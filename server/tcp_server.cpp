#include "tcp_server.hpp"

#include <cstdio>
#include <cstring>
#include <sys/socket.h>

#include "acceptor.hpp"
#include "event_loop.hpp"
#include "event_loop_thread_pool.hpp"

TcpServer::TcpServer(EventLoop* loop, const struct sockaddr_in& listenAddr, std::string name)
    : loop_(loop),
      name_(std::move(name)),
      acceptor_(std::make_unique<Acceptor>(loop, listenAddr, true)),
      threadPool_(std::make_unique<EventLoopThreadPool>(loop, name_)),
      nextConnId_(1),
      started_(false) {
    acceptor_->setNewConnectionCallback(
        [this](int sockfd, const struct sockaddr_in& peerAddr) {
            newConnection(sockfd, peerAddr);
        });
}

TcpServer::~TcpServer() {
    for (auto& kv : connections_) {
        TcpConnectionPtr conn = kv.second;
        kv.second.reset();
        conn->getLoop()->runInLoop([conn]() { conn->connectDestroyed(); });
    }
}

void TcpServer::setThreadNum(int numThreads) {
    threadPool_->setThreadNum(numThreads);
}

void TcpServer::start() {
    if (started_) return;
    started_ = true;
    threadPool_->start();   // 先把 IO 线程池拉起来
    acceptor_->listen();    // 再开始监听
}

void TcpServer::newConnection(int sockfd, const struct sockaddr_in& peerAddr) {
    loop_->assertInLoopThread();

    // 轮询挑一个 IO 线程（numThreads=0 时就是 baseLoop 自己）
    EventLoop* ioLoop = threadPool_->getNextLoop();

    char buf[32];
    ::snprintf(buf, sizeof(buf), "-%05d", nextConnId_);
    ++nextConnId_;
    const std::string connName = name_ + buf;

    struct sockaddr_in localAddr;
    std::memset(&localAddr, 0, sizeof(localAddr));
    socklen_t addrlen = sizeof(localAddr);
    ::getsockname(sockfd, reinterpret_cast<struct sockaddr*>(&localAddr), &addrlen);

    TcpConnectionPtr conn =
        std::make_shared<TcpConnection>(ioLoop, connName, sockfd, localAddr, peerAddr);

    connections_[connName] = conn;
    conn->setConnectionCallback(connectionCallback_);
    conn->setMessageCallback(messageCallback_);
    conn->setCloseCallback([this](const TcpConnectionPtr& c) { removeConnection(c); });

    // 关键：连接要在它所属的 IO 线程里"成立"（注册 epoll 读事件）
    ioLoop->runInLoop([conn]() { conn->connectEstablished(); });
}

void TcpServer::removeConnection(const TcpConnectionPtr& conn) {
    // 可能来自 IO 线程；connections_ 只在 baseLoop 访问，所以先切回 baseLoop
    loop_->runInLoop([this, conn]() { removeConnectionInLoop(conn); });
}

void TcpServer::removeConnectionInLoop(const TcpConnectionPtr& conn) {
    loop_->assertInLoopThread();
    connections_.erase(conn->name());

    // 销毁动作排到连接的 IO 线程：此时已离开 Channel::handleEvent()，析构才安全
    EventLoop* ioLoop = conn->getLoop();
    ioLoop->queueInLoop([conn]() { conn->connectDestroyed(); });
}
