#pragma once

// ============================================================================
//  TcpConnection —— 一个 TCP 连接的完整生命周期
//
//  职责：
//    - 连接的建立 / 关闭；
//    - 读：数据到达 -> 读进 inputBuffer_ -> 唤醒等待的协程（或回调 messageCallback_）；
//    - 写：先尝试直接写，写不完缓存到 outputBuffer_ 并注册 EPOLLOUT；
//    - 生命周期：必须用 shared_ptr 管理，因为它会被多个回调 / 协程持有。
//
//  ── 协程读写接口 ────────────────────────────────────────────────
//    co_await conn->asyncRead()        -> 挂起直到有数据，返回读到的字节（空串=EOF）
//    co_await conn->asyncWrite("...")  -> 挂起直到写完
//
//  ── 「超时」与「取消」 ──────────────────────────────────────────
//    co_await conn->asyncRead().withTimeout(5s)     -> 超时抛 OperationTimeout
//    连接关闭 / 销毁时，挂起的读写操作会被唤醒并抛 OperationCancelled
//    实现：每次挂起创建一份 CoroOpState（堆上、shared_ptr 共享），
//    "epoll 回调 / 超时定时器 / 关闭取消"三个唤醒源谁先到谁裁决（done 标记），
//    彻底杜绝"重复 resume"与"晚到定时器踩悬垂句柄"两类 bug。
//
//  ── 「聚集写」 ──────────────────────────────────────────────────
//    co_await conn->asyncWriteSegments({header, body})
//    HTTP 响应天然是"头 + 体"两段，用一次 writev 发出去，
//    省掉"把头拼到体前面"的 O(体长) memcpy。
// ============================================================================

#include <chrono>
#include <coroutine>
#include <functional>
#include <memory>
#include <netinet/in.h>
#include <string>
#include <utility>
#include <vector>

#include "buffer.hpp"
#include "common.hpp"
#include "coro_error.hpp"

class EventLoop;
class Channel;

class TcpConnection;
using TcpConnectionPtr = std::shared_ptr<TcpConnection>;

class TcpConnection : NonCopyable, public std::enable_shared_from_this<TcpConnection> {
public:
    using ConnectionCallback = std::function<void(const TcpConnectionPtr&)>;
    using MessageCallback = std::function<void(const TcpConnectionPtr&, Buffer*)>;
    using CloseCallback = std::function<void(const TcpConnectionPtr&)>;

    // ------------------------------------------------------------------
    //  协程 Awaiter（实现放 .cpp，避免头文件依赖不完整类型）
    // ------------------------------------------------------------------

    // 读 Awaiter：co_await conn->asyncRead()[.withTimeout(3s)]
    class ReadAwaiter {
    public:
        explicit ReadAwaiter(TcpConnection* conn, double timeoutSeconds = 0.0)
            : conn_(conn), timeoutSeconds_(timeoutSeconds) {}

        bool await_ready() const noexcept;
        void await_suspend(std::coroutine_handle<> h);
        std::string await_resume();

        // 链式设置超时：co_await conn->asyncRead().withTimeout(5s)
        ReadAwaiter withTimeout(std::chrono::steady_clock::duration d) && {
            timeoutSeconds_ = std::chrono::duration<double>(d).count();
            return std::move(*this);
        }

    private:
        TcpConnection* conn_;
        double timeoutSeconds_ = 0.0;   // <=0 表示不超时
        CoroOpStatePtr op_;             // 本次挂起的状态（挂起后才非空）
    };

    // 写 Awaiter：co_await conn->asyncWrite(data)[.withTimeout(3s)]
    class WriteAwaiter {
    public:
        WriteAwaiter(TcpConnection* conn, std::string data)
            : conn_(conn), data_(std::move(data)) {}

        bool await_ready() const noexcept { return false; }  // 统一走挂起路径
        void await_suspend(std::coroutine_handle<> h);
        void await_resume();

        WriteAwaiter withTimeout(std::chrono::steady_clock::duration d) && {
            timeoutSeconds_ = std::chrono::duration<double>(d).count();
            return std::move(*this);
        }

    private:
        TcpConnection* conn_;
        std::string data_;
        double timeoutSeconds_ = 0.0;
        CoroOpStatePtr op_;
    };

    // 聚集写 Awaiter：co_await conn->asyncWriteSegments({header, body})[.withTimeout(3s)]
    //
    //  为什么需要它：HTTP 响应是"一段小的头部 + 一段可能很大的体"。
    //  若拼接成一整串再 write，就要为体做一次全量拷贝；写成两个 write 又变两次系统调用。
    //  writev 一次调用把多段内核可见地拼起来发走，两边的开销都省了。
    //
    //  语义与 WriteAwaiter 完全一致（挂起直到写完 / 超时 / 取消），
    //  唯一区别是"要写的内容"是多段。段在**挂起期间**必须存活，
    //  所以这里按值持有 vector<string>（它就在协程帧里，随挂起一起保活）；
    //  写不完的部分会被拷进 outputBuffer_，调用方之后可以放心销毁自己的字符串。
    class WriteSegmentsAwaiter {
    public:
        WriteSegmentsAwaiter(TcpConnection* conn, std::vector<std::string> segs)
            : conn_(conn), segs_(std::move(segs)) {}

        bool await_ready() const noexcept { return false; }  // 统一走挂起路径
        void await_suspend(std::coroutine_handle<> h);
        void await_resume();

        WriteSegmentsAwaiter withTimeout(std::chrono::steady_clock::duration d) && {
            timeoutSeconds_ = std::chrono::duration<double>(d).count();
            return std::move(*this);
        }

    private:
        TcpConnection* conn_;
        std::vector<std::string> segs_;
        double timeoutSeconds_ = 0.0;
        CoroOpStatePtr op_;
    };

    ReadAwaiter asyncRead() { return ReadAwaiter(this, 0.0); }
    WriteAwaiter asyncWrite(std::string data) { return WriteAwaiter(this, std::move(data)); }
    WriteSegmentsAwaiter asyncWriteSegments(std::vector<std::string> segs) {
        return WriteSegmentsAwaiter(this, std::move(segs));
    }

    // ------------------------------------------------------------------

    TcpConnection(EventLoop* loop, std::string name, int sockfd,
                  const struct sockaddr_in& localAddr, const struct sockaddr_in& peerAddr);
    ~TcpConnection();

    EventLoop* getLoop() const { return loop_; }
    const std::string& name() const { return name_; }
    bool connected() const { return state_ == kConnected; }

    void setConnectionCallback(ConnectionCallback cb) { connectionCallback_ = std::move(cb); }
    void setMessageCallback(MessageCallback cb) { messageCallback_ = std::move(cb); }
    void setCloseCallback(CloseCallback cb) { closeCallback_ = std::move(cb); }

    void connectEstablished();  // 连接建立：开始关注可读事件
    void connectDestroyed();    // 连接销毁：唤醒挂起协程 + 从 epoll 摘除

    void send(const std::string& message);
    void shutdown();

private:
    enum StateE { kDisconnected, kConnecting, kConnected, kDisconnecting };

    void setState(StateE s) { state_ = s; }

    void handleRead();
    void handleWrite();
    void handleClose();
    void handleError();
    void sendInLoop(const std::string& message);
    void shutdownInLoop();

    // ---- 协程读写操作的启动与裁决 ----
    void startReadOp(const CoroOpStatePtr& op, double timeoutSeconds);
    void startWriteOp(const CoroOpStatePtr& op, std::string data, double timeoutSeconds);
    void startWriteSegmentsOp(const CoroOpStatePtr& op, std::vector<std::string> segs,
                              double timeoutSeconds);
    // 把多段内容尽量用一次 writev 发出；写不完的按字节位置落回 outputBuffer_
    void sendSegmentsInLoop(const std::vector<std::string>& segs);
    // 裁决一次读/写操作：第一个调用者生效，其余忽略。cancelled/timedOut 决定协程的归宿。
    void finishReadOp(CoroOpStatePtr op, bool cancelled, bool timedOut);
    void finishWriteOp(CoroOpStatePtr op, bool cancelled, bool timedOut);
    void flushOutput();  // 尽量把 outputBuffer_ 写给内核

    EventLoop* loop_;
    const std::string name_;
    StateE state_;
    std::unique_ptr<Channel> channel_;
    std::unique_ptr<Buffer> inputBuffer_;
    std::unique_ptr<Buffer> outputBuffer_;
    ConnectionCallback connectionCallback_;
    MessageCallback messageCallback_;
    CloseCallback closeCallback_;

    CoroOpStatePtr readOp_;   // 当前挂起的读操作（空 = 无）
    CoroOpStatePtr writeOp_;  // 当前挂起的写操作（空 = 无）
};
