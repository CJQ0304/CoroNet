#pragma once

// ============================================================================
//  TcpServer —— 主从 Reactor 组装层
//
//  线程模型：
//      main thread (baseLoop_)  : 只管 accept，不做业务
//            |  轮询分发新连接
//            v
//      IO thread 0..N (subLoop) : 每个连接固定绑在其中一个线程
//
//  connections_ 表只在 baseLoop 线程访问，因此不需要加锁。
//
//  线程安全要点：
//    - 新连接在 baseLoop 上创建，但要 connectEstablished 到它所属的 IO loop；
//    - removeConnection 可能来自 IO 线程，必须先回到 baseLoop 再改表；
//    - 真正销毁连接要 queueInLoop 延迟执行，避免在 handleEvent 内部析构。
// ============================================================================

#include <cstddef>
#include <map>
#include <memory>
#include <netinet/in.h>
#include <string>

#include "common.hpp"
#include "tcp_connection.hpp"

class EventLoop;
class Acceptor;
class EventLoopThreadPool;

class TcpServer : NonCopyable {
public:
    TcpServer(EventLoop* loop, const struct sockaddr_in& listenAddr, std::string name);
    ~TcpServer();

    void setThreadNum(int numThreads);  // 必须在 start() 之前调用
    void start();

    // ---- 供"周期性统计"读取的连接信息 ----
    // ⚠️ connections_ 只在 baseLoop 线程被修改，所以这两个方法也必须在
    //    baseLoop 线程（例如 EventLoop::runEvery 的回调）里调用才安全。
    size_t connectionCount() const { return connections_.size(); }  // 当前活跃连接数
    long totalAccepted() const { return nextConnId_ - 1; }          // 累计接受过的连接数

    void setConnectionCallback(TcpConnection::ConnectionCallback cb) {
        connectionCallback_ = std::move(cb);
    }
    void setMessageCallback(TcpConnection::MessageCallback cb) {
        messageCallback_ = std::move(cb);
    }

private:
    void newConnection(int sockfd, const struct sockaddr_in& peerAddr);
    void removeConnection(const TcpConnectionPtr& conn);        // 任意线程可调
    void removeConnectionInLoop(const TcpConnectionPtr& conn);  // 只在 baseLoop

    EventLoop* loop_;  // baseLoop_
    const std::string name_;
    std::unique_ptr<Acceptor> acceptor_;
    std::unique_ptr<EventLoopThreadPool> threadPool_;
    TcpConnection::ConnectionCallback connectionCallback_;
    TcpConnection::MessageCallback messageCallback_;
    std::map<std::string, TcpConnectionPtr> connections_;
    int nextConnId_;
    bool started_;
};
