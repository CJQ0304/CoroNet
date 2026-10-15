// ============================================================================
//  echo_server.cpp —— 回调式 echo 服务
//
//  运行后监听 8888 端口，把收到的任何数据原样回显（echo）。
//  测试：
//    telnet 127.0.0.1 8888     # 或 nc 127.0.0.1 8888
//    输入任何文字，回车，应当立即看到同样内容返回。
// ============================================================================

#include <arpa/inet.h>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>

#include "buffer.hpp"
#include "event_loop.hpp"
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

void onConnection(const TcpConnectionPtr& conn) {
    LOG_INFO("connection callback [" << conn->name() << "]");
}

void onMessage(const TcpConnectionPtr& conn, Buffer* buf) {
    std::string msg = buf->retrieveAllAsString();
    LOG_INFO("[" << conn->name() << "] recv " << msg.size() << " bytes");
    conn->send(msg);  // echo 回去
}

// 优雅退出（Ctrl+C / kill 也能把异步日志刷完，详见 coro_echo_server.cpp 注释）
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

    // 异步日志（必须在创建任何线程之前初始化）
    coronet::Logger::Options logOpt;
    logOpt.basename = "coronet-echo-cb";
    logOpt.rollSize = static_cast<size_t>(64) * 1024 * 1024;
    logOpt.flushInterval = 0.5;
    logOpt.console = (std::getenv("CORONET_LOG_QUIET") == nullptr);
    coronet::Logger::init(logOpt);

    const struct sockaddr_in listenAddr = makeAddress("0.0.0.0", port);

    EventLoop loop;
    g_loop = &loop;
    ::signal(SIGINT, onTerminate);
    ::signal(SIGTERM, onTerminate);

    TcpServer server(&loop, listenAddr, "EchoServer");
    server.setThreadNum(numThreads);   // ★ IO 线程数（0 = 单线程）
    server.setConnectionCallback(onConnection);
    server.setMessageCallback(onMessage);
    server.start();

    LOG_INFO("EchoServer listening on 0.0.0.0:" << port << " with " << numThreads << " IO threads");
    loop.loop();

    coronet::Logger::flush();
    coronet::Logger::shutdown();   // 停后端线程 + 刷盘
    return 0;
}
