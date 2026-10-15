// ============================================================================
//  http_server.cpp —— 跑在协程上的 HTTP/1.1 服务
//
//  与回调式写法对比，协程的价值在于把"一个请求的完整流程"还原成顺序代码：
//
//    回调式：读事件 -> onMessage -> 业务被切碎；想"读完一个完整请求再回复"
//            就得自己维护状态机 + 缓冲区，散落在回调之间；
//    协程式：一个连接一个协程，顺序写着"读一包 -> 解析 -> 回包 -> 再读"，
//            解析器的状态与缓冲自然成为协程局部变量 —— 不需要任何成员变量，
//            也不需要"回调式状态机"那套手写续延。
//
//  协程挂起期间，它所在的 IO 线程不阻塞，继续服务别的连接。这就是
//  "用同步写法拿异步性能"。压测数据见 README 的「性能」一节。
//
//  路由（故意只留能验证功能的最小集合）：
//    GET  /            一段 HTML 首页
//    GET  /plain       最小响应 "OK"（压测用，避免把解析/路由外的因素带进来）
//    GET  /hello?msg=x 回显 query 参数
//    GET  /echo?msg=x  同上（GET 版）
//    POST /echo        回显请求体（验证 Content-Length 与 body 解析）
//    其它              404；非 GET/POST -> 405
//
//  测试：
//    ./http_server 8080 4
//    curl -v http://127.0.0.1:8080/plain
//    curl -v "http://127.0.0.1:8080/hello?msg=CoroNet"
//    curl -v -d 'body-data' http://127.0.0.1:8080/echo
//    wrk -t4 -c100 -d10s http://127.0.0.1:8080/plain
// ============================================================================

#include <arpa/inet.h>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "buffer.hpp"
#include "common.hpp"
#include "event_loop.hpp"
#include "http_parser.hpp"
#include "http_response.hpp"
#include "task.hpp"
#include "tcp_connection.hpp"
#include "tcp_server.hpp"

// http_* 三件套在 coronet 命名空间里（日志模块也在），这里显式引入，
// 比 "using namespace coronet" 更克制 —— 一眼能看出用到了哪些东西。
using coronet::HttpContext;
using coronet::HttpMethod;
using coronet::HttpRequest;
using coronet::HttpResponse;
using coronet::HttpStatus;
using coronet::HttpVersion;
using coronet::iequals;

namespace {

// 空闲超时 10 秒（比 echo 的 5 秒宽松些：浏览器/压测客户端可能有一段时间不发数据）
constexpr auto kIdleTimeout = std::chrono::seconds(10);
// 写超时 5 秒：防止对端只连不读，把 outputBuffer_ 一直拖住
constexpr auto kWriteTimeout = std::chrono::seconds(5);

// ---------------------------------------------------------------------------
//  跨线程统计
//
//  请求发生在**各个 IO 线程**，而统计日志打在 **baseLoop 线程**。
//  这是本项目里少数几个真的跨线程共享的数据，用原子量解决：
//  memory_order_relaxed 就够了 —— 我们只关心计数本身，不需要用它去
//  "同步"别的内存访问。这是"能不加锁就不加锁"的一个具体落点。
// ---------------------------------------------------------------------------
std::atomic<uint64_t> g_requests{0};
std::atomic<uint64_t> g_errors{0};

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

constexpr char kIndexHtml[] =
    "<!DOCTYPE html>\n"
    "<html lang=\"zh-CN\"><head><meta charset=\"utf-8\">"
    "<title>CoroNet</title></head><body>\n"
    "<h2>CoroNet HTTP server</h2>\n"
    "<p>C++20 coroutine + multi-threaded Reactor.</p>\n"
    "<ul>\n"
    "  <li><a href=\"/plain\">/plain</a> - minimal body (for benchmark)</li>\n"
    "  <li><a href=\"/hello?msg=CoroNet\">/hello?msg=xxx</a> - echo query</li>\n"
    "</ul>\n"
    "</body></html>\n";

void setPlainBody(HttpResponse* resp, std::string body) {
    resp->setContentType("text/plain; charset=utf-8");
    resp->setBody(std::move(body));
}

// ---------------------------------------------------------------------------
//  路由：把请求翻译成响应
//
//  注意这个函数是**纯同步**的：它不碰 IO、不 co_await。协程的价值只体现在
//  "等数据"这件事上；一旦数据齐了，处理逻辑就该是普通函数 —— 这样它才好单测。
// ---------------------------------------------------------------------------
void handleRequest(const HttpRequest& req, HttpResponse* resp) {
    g_requests.fetch_add(1, std::memory_order_relaxed);

    // ---- keep-alive 判定 ----
    //   HTTP/1.1：默认长连接，只有显式 `Connection: close` 才关；
    //   HTTP/1.0：默认短连接，只有显式 `Connection: keep-alive` 才留。
    // 字段名和值都大小写不敏感（所以用 iequals 而不是 ==）。
    bool close = true;
    if (req.version() == HttpVersion::kHttp11) {
        close = iequals(req.getHeader("Connection"), "close");
    } else {
        close = !iequals(req.getHeader("Connection"), "keep-alive");
    }
    resp->clear(close);

    // ---- 方法过滤 ----
    if (req.method() != HttpMethod::kGet && req.method() != HttpMethod::kPost) {
        resp->setStatusCode(HttpStatus::k405MethodNotAllowed);
        resp->addHeader("Allow", "GET, POST");
        setPlainBody(resp, "405 Method Not Allowed\n");
        return;
    }

    const std::string& path = req.path();

    if (req.method() == HttpMethod::kGet) {
        if (path == "/") {
            resp->setContentType("text/html; charset=utf-8");
            resp->setBody(kIndexHtml);
            return;
        }
        if (path == "/plain") {
            // 压测端：响应体最小，尽量把变量收敛到"网络栈 + 日志"上
            setPlainBody(resp, "OK");
            return;
        }
        if (path == "/hello" || path == "/echo") {
            const std::string_view msg = req.getQueryParam("msg");
            setPlainBody(resp, msg.empty() ? std::string("(missing msg)") : std::string(msg));
            return;
        }
    } else {   // POST
        if (path == "/echo") {
            setPlainBody(resp, req.body());   // 回显 body，验证 Content-Length 解析
            return;
        }
    }

    resp->setStatusCode(HttpStatus::k404NotFound);
    setPlainBody(resp, "404 Not Found\n");
}

// ---------------------------------------------------------------------------
//  解析失败 -> 错误响应
//
//  一律 `close = true`：报文已经非法，字节边界不可信，继续在这个连接上
//  读下一个请求可能把"上一个请求的尾巴"当成新请求的开头。
//  这是安全上的常见做法（也顺带避免了"同一连接反复发坏包拖住资源"）。
// ---------------------------------------------------------------------------
void buildErrorResponse(int status, const std::string& message, HttpResponse* resp) {
    resp->clear(true);
    resp->setStatusMessage(std::string());          // 用默认原因短语
    resp->setStatusCode(static_cast<HttpStatus>(status));
    setPlainBody(resp, message + "\n");
    g_errors.fetch_add(1, std::memory_order_relaxed);
}

// ---------------------------------------------------------------------------
//  ★ 核心：一个连接一个协程
//
//  参数 conn 按值传入 -> 被拷贝进协程执行帧 -> 协程存活期间连接不会被销毁。
// ---------------------------------------------------------------------------
DetachedTask handleHttpConnection(TcpConnectionPtr conn) {
    // 每连接三件"可复用对象"，全部是协程的局部变量（在协程帧里）：
    //   in   —— 解析缓冲：装"收到但还没解析完"的字节（解决半包）
    //   ctx  —— 解析状态机：keep-alive 下用 reset() 复用
    //   resp —— 响应对象：clear() 保留 capacity，稳态零堆分配
    // 因为一个连接固定绑在一个 IO 线程上，这三样天然不需要任何同步。
    //
    // 放在这里（而不是 TcpConnection 的成员里）还有个好处：
    // 连接对象完全不需要知道"HTTP"这件事，网络层与协议层干净解耦。
    Buffer in;
    HttpContext ctx;
    HttpResponse resp;

    LOG_INFO("[http] begin " << conn->name() << " tid=" << std::this_thread::get_id());

    bool keepRunning = true;
    try {
        while (keepRunning && conn->connected()) {
            // 挂起，直到有新数据 / 对端关闭 / 超时
            std::string chunk = co_await conn->asyncRead().withTimeout(kIdleTimeout);
            if (chunk.empty()) {
                break;   // EOF：对端关闭
            }
            in.append(chunk.data(), chunk.size());

            // 一个 TCP 包里可能装着多个请求（HTTP 流水线），
            // 所以要把缓冲"榨干"再回去读下一个包。
            while (conn->connected()) {
                const HttpContext::ParseResult result = ctx.parse(&in);
                if (result == HttpContext::ParseResult::kIncomplete) {
                    break;   // 半个请求，回去接着读
                }

                if (result == HttpContext::ParseResult::kError) {
                    buildErrorResponse(ctx.errorStatus(), ctx.errorMessage(), &resp);
                } else {
                    handleRequest(ctx.request(), &resp);
                }

                // ---- 组装响应：头部一段 + 体一段 ----
                // 顺序不能反：appendHeaderTo 要用 body_.size() 填 Content-Length，
                // 所以必须先把头组装好，再 takeBody() 把体**移动**出来（零拷贝）。
                std::string header;
                resp.appendHeaderTo(&header);
                std::string body = resp.takeBody();

                std::vector<std::string> segs;
                segs.reserve(2);
                segs.push_back(std::move(header));
                if (!body.empty()) {
                    segs.push_back(std::move(body));
                }

                // 一次 writev 把两段发出去（而不是"拼成一整串再 write"）
                co_await conn->asyncWriteSegments(std::move(segs)).withTimeout(kWriteTimeout);

                if (resp.closeConnection()) {
                    keepRunning = false;
                    break;
                }
                ctx.reset();          // 准备解析同一连接上的下一个请求
                resp.clear(false);    // 复用响应对象
            }
        }
    } catch (const OperationTimeout&) {
        LOG_INFO("[http] " << conn->name() << " idle timeout -> close");
    } catch (const OperationCancelled&) {
        LOG_INFO("[http] " << conn->name() << " cancelled -> close");
    }

    conn->shutdown();
    LOG_INFO("[http] end " << conn->name());
}

void onConnection(const TcpConnectionPtr& conn) {
    if (conn->connected()) {
        handleHttpConnection(conn);   // 启动这个连接的协程
    } else {
        LOG_INFO("[server] connection gone " << conn->name());
    }
}

// ---------------------------------------------------------------------------
//  优雅退出：信号处理里只做"异步信号安全"的两件事
//    1) quit()   置退出标志
//    2) wakeup() 往 eventfd 写 8 字节，把阻塞在 epoll_wait 的 loop 立刻叫醒
//  这样 loop.loop() 能正常返回，后面的 Logger::flush/shutdown 才执行得到
//  （异步日志若被硬杀会丢最后一批）。
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
    uint16_t port = 8080;                 // HTTP 默认用 8080，避免和 echo 的 8888 冲突
    int numThreads = 4;
    if (argc > 1) {
        port = static_cast<uint16_t>(std::atoi(argv[1]));
    }
    if (argc > 2) {
        numThreads = std::atoi(argv[2]);
    }

    // 异步日志必须**在创建任何线程之前**初始化
    coronet::Logger::Options logOpt;
    logOpt.basename = "coronet-http";
    logOpt.rollSize = static_cast<size_t>(64) * 1024 * 1024;
    logOpt.flushInterval = 0.5;
    // 压测时务必关掉终端镜像（镜像由后端线程做，但 stderr 写多了仍是开销）：
    //   CORONET_LOG_QUIET=1 ./http_server 8080 4
    logOpt.console = (std::getenv("CORONET_LOG_QUIET") == nullptr);
    coronet::Logger::init(logOpt);

    const struct sockaddr_in listenAddr = makeAddress("0.0.0.0", port);

    EventLoop loop;
    g_loop = &loop;
    ::signal(SIGINT, onTerminate);
    ::signal(SIGTERM, onTerminate);

    TcpServer server(&loop, listenAddr, "HttpServer");
    server.setThreadNum(numThreads);
    server.setConnectionCallback(onConnection);
    // 协程模式下**不设置 messageCallback_**：读事件直接唤醒等待中的协程。
    server.start();

    // 定时器做周期统计：顺手算出"最近 5 秒的 RPS"，和 wrk 的数字互相印证。
    loop.runEvery(5.0, [&server, lastReq = uint64_t{0}, lastErr = uint64_t{0}]() mutable {
        const uint64_t req = g_requests.load(std::memory_order_relaxed);
        const uint64_t err = g_errors.load(std::memory_order_relaxed);
        LOG_INFO("[stats] active connections = " << server.connectionCount()
                 << ", total accepted = " << server.totalAccepted()
                 << ", requests = " << req << " (last 5s = " << (req - lastReq)
                 << ", rps = " << (req - lastReq) / 5 << ")"
                 << ", errors = " << err << " (last 5s = " << (err - lastErr) << ")");
        lastReq = req;
        lastErr = err;
    });

    LOG_INFO("HttpServer listening on 0.0.0.0:" << port << " with " << numThreads
                                               << " IO threads (coroutine mode)");
    LOG_INFO("try: curl -v http://127.0.0.1:" << port << "/plain");
    loop.loop();

    LOG_INFO("server exiting, flushing remaining logs...");
    coronet::Logger::flush();
    coronet::Logger::shutdown();
    return 0;
}
