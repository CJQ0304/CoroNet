#pragma once

// ============================================================================
//  Acceptor —— 负责"监听 + 接受新连接"
//
//  内部持有一个监听 socket 和一个 Channel：
//  监听 fd 可读 = 有新连接到来 -> handleRead 里 accept4 取出连接，
//  再通过 newConnectionCallback_ 交给 TcpServer 去创建 TcpConnection。
//
//  用 accept4 一次拿到"非阻塞 + close-on-exec"的连接 fd，省去两次 fcntl。
// ============================================================================

#include <functional>
#include <memory>
#include <netinet/in.h>

#include "common.hpp"

class EventLoop;
class Channel;

class Acceptor : NonCopyable {
public:
    using NewConnectionCallback =
        std::function<void(int sockfd, const struct sockaddr_in& peerAddr)>;

    Acceptor(EventLoop* loop, const struct sockaddr_in& listenAddr, bool reusePort);
    ~Acceptor();

    void listen();

    void setNewConnectionCallback(NewConnectionCallback cb) {
        newConnectionCallback_ = std::move(cb);
    }

private:
    void handleRead();  // 监听 fd 可读 -> accept

    EventLoop* loop_;
    int listenFd_;
    std::unique_ptr<Channel> acceptChannel_;
    NewConnectionCallback newConnectionCallback_;
    bool listening_;
};
