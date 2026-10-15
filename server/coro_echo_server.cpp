// ============================================================================
//  coro_echo_server.cpp —— 基于 C++20 协程的 echo 服务
//
//  与回调式的 echo_server.cpp 对比：
//    - 回调试：onMessage 回调被反复触发，业务逻辑被切碎在回调里（回调地狱）；
//    - 协程式：**一个连接 = 一个协程**，用"同步"的顺序写法表达异步逻辑——
//             读 -> 处理 -> 写 -> 再读，一眼看懂，没有嵌套。
//
//  关键点：协程挂起期间，其所属的 IO 线程**不阻塞**、继续处理别的连接。
//          这就是"用同步写法拿异步性能"。
//
//  测试：
//    ./coro_echo_server 8888 4
//    nc 127.0.0.1 8888       # 输入文字回车，等 ~200ms 看到 "echo: xxx"
// ============================================================================

#include <arpa/inet.h>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>

#include "coro_error.hpp"
#include "event_loop.hpp"
#include "sleep.hpp"
#include "task.hpp"
#include "tcp_connection.hpp"
#include "tcp_server.hpp"

namespace {

struct sockaddr_in makeAddress(const char* ip, uint16_t port) {
    struct sockaddr_in addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (::inet_pton(AF_INET, ip, &addr.sin_addr) <= 0) {
        LOG_ERROR("invalid ip: " << ip);
        std::abort();
    }
    return addr;
}

// ---------------------------------------------------------------------------
//  ★ 核心：一个连接一个协程。返回 DetachedTask = 立即启动、结束自毁。
//
//  参数 conn 按值传入 -> 被拷贝进协程执行帧 -> 协程存活期间连接不会被销毁。
//  try/catch 覆盖超时 / 取消：它们不再是"静默卡死"，而是抛异常，
//  用同步写法捕获，语义清晰。
// ---------------------------------------------------------------------------
DetachedTask handleConnection(TcpConnectionPtr conn) {
    // 空闲超时：5 秒没读到数据就主动断开（防止"僵尸连接"长期占用资源）
    constexpr auto kIdleTimeout = std::chrono::seconds(5);

    LOG_INFO("[coro] begin " << conn->name() << " tid=" << std::this_thread::get_id());

    try {
        while (conn->connected()) {
            // 1) 挂起，直到有数据 / 对端关闭 / 超时
            std::string data = co_await conn->asyncRead().withTimeout(kIdleTimeout);
            if (data.empty()) {
                LOG_INFO("[coro] " << conn->name() << " EOF -> exit");
                break;
            }
            LOG_INFO("[coro] " << conn->name() << " recv " << data.size() << " bytes");

            // 2) 模拟耗时业务：挂起 200ms，期间线程照样服务其他连接
            co_await sleepFor(conn->getLoop(), std::chrono::milliseconds(200));

            // 3) 挂起，直到写完（带超时，防止对端只读不写把连接拖死）
            co_await conn->asyncWrite("echo: " + data).withTimeout(std::chrono::seconds(3));
        }
    } catch (const OperationTimeout&) {
        LOG_INFO("[coro] " << conn->name() << " idle timeout -> close");
    } catch (const OperationCancelled&) {
        LOG_INFO("[coro] " << conn->name() << " cancelled -> close");
    }

    conn->shutdown();
    LOG_INFO("[coro] end " << conn->name());
}

void onConnection(const TcpConnectionPtr& conn) {
    if (conn->connected()) {
        // 启动协程处理这个连接（同步写法，异步执行）
        handleConnection(conn);
    } else {
        LOG_INFO("[server] connection gone " << conn->name());
    }
}

// ---------------------------------------------------------------------------
//  优雅退出（让 Ctrl+C 也能把日志刷完）
//
//  信号处理函数里能安全做的事极少，这里只做两件：
//    1) quit()   置退出标志；
//    2) wakeup() 往 eventfd 写 8 字节 —— ::write 是"异步信号安全"函数，
//       保证即使 loop 线程正阻塞在 epoll_wait，也能立刻醒来看见退出标志。
//  于是 loop.loop() 正常返回，后面的"刷日志 + 停后端线程"才真正执行得到。
//
//  为什么强调这一点：异步日志的日志在内存里，进程被"硬杀"就丢了；
//  只有走完整退出流程，才能保证最后一批日志落盘。
// ---------------------------------------------------------------------------
EventLoop* g_loop = nullptr;

void onTerminate(int /*signum*/) {
    if (g_loop != nullptr) {
        g_loop->quit();
        g_loop->wakeup();
    }
}

}  // namespace

int main(int argc, char** argv) {
    uint16_t port = 8888;
    int numThreads = 4;
    if (argc > 1) {
        port = static_cast<uint16_t>(std::atoi(argv[1]));
    }
    if (argc > 2) {
        numThreads = std::atoi(argv[2]);
    }

    // ---- 异步日志 ----------------------------------------------------------
    // 必须**在创建任何线程之前**初始化：后端线程一跑起来就开始接管日志，
    // 晚初始化会把服务启动期的日志漏掉。
    // console=true 时日志会同时镜像到终端（由**后端线程**输出，业务线程依旧零 I/O）；
    // 压测时用 `CORONET_LOG_QUIET=1` 关掉镜像，才能测出异步日志的真实开销。
    coronet::Logger::Options logOpt;
    logOpt.basename = "coronet-echo";
    logOpt.rollSize = static_cast<size_t>(64) * 1024 * 1024;
    // 调试期用 0.5s：日志约每半秒成批出现，肉眼能跟得上；
    // 生产可调到 2~3s，让"攒批"效果更明显、落盘次数更少。
    logOpt.flushInterval = 0.5;
    logOpt.console = (std::getenv("CORONET_LOG_QUIET") == nullptr);
    coronet::Logger::init(logOpt);

    const struct sockaddr_in listenAddr = makeAddress("0.0.0.0", port);

    EventLoop loop;
    g_loop = &loop;
    ::signal(SIGINT, onTerminate);    // Ctrl+C
    ::signal(SIGTERM, onTerminate);   // kill

    TcpServer server(&loop, listenAddr, "CoroServer");
    server.setThreadNum(numThreads);
    server.setConnectionCallback(onConnection);
    // 注意：协程模式下**不设置 messageCallback_**，
    // 读事件会直接唤醒等待中的协程，而不是走回调。
    server.start();

    // ---- 定时器驱动的周期性统计 --------------------------------------------
    // 回调跑在 baseLoop 线程，与 connections_ 的访问线程一致，所以不必加锁。
    // 这正是"定时器 + 事件循环"在服务端的典型用法：心跳、统计、超时巡检。
    loop.runEvery(5.0, [&server]() {
        LOG_INFO("[stats] active connections = " << server.connectionCount()
                                                 << ", total accepted = " << server.totalAccepted());
    });

    LOG_INFO("CoroEchoServer listening on 0.0.0.0:" << port << " with " << numThreads
                                                   << " IO threads (coroutine mode)");
    loop.loop();

    LOG_INFO("server exiting, flushing remaining logs...");
    coronet::Logger::flush();
    coronet::Logger::shutdown();   // 停后端线程 + 把残留日志刷盘
    return 0;
}
