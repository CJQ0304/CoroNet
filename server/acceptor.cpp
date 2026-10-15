#include "acceptor.hpp"

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <netinet/in.h>
#include <netinet/tcp.h>   // TCP_NODELAY
#include <sys/socket.h>
#include <unistd.h>

#include "channel.hpp"
#include "event_loop.hpp"

namespace {

// 创建监听 socket：直接带上 NONBLOCK + CLOEXEC（Linux 专有 flag）
int createNonblockingSocket() {
    const int sockfd = ::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, IPPROTO_TCP);
    if (sockfd < 0) {
        LOG_ERROR("socket() failed: " << std::strerror(errno));
        std::abort();
    }
    return sockfd;
}

void setReuseAddr(int fd, bool on) {
    const int optval = on ? 1 : 0;
    ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval));
}

void setReusePort(int fd, bool on) {
#ifdef SO_REUSEPORT
    const int optval = on ? 1 : 0;
    ::setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &optval, sizeof(optval));
#else
    (void)fd;
    (void)on;
#endif
}

// 连接级选项：禁用 Nagle 算法。
//
// 为什么必须设（这是实测踩出来的，不是照抄惯例）：
//   Nagle 的规则是「只要还有未被 ACK 的在途数据，就先把小包攒住」。
//   于是"先写头、再写体"这种**分两次写**的响应会陷进经典交互：
//     ① 头那段先发出去；
//     ② 体那段 < MSS，且前一段尚未被 ACK ⟹ 被 Nagle 攒住；
//     ③ 对端只收到半个响应，解析不出完整内容，触发 delayed ACK（Linux 最坏 40ms）；
//     ④ 40ms 后 ACK 到达，体才发出去。
//   本项目实测：`http_server_nowritev`（把 writev 退化成分段 write）在不设本选项时，
//   每请求延迟 80µs → 44ms、QPS 100 万 → 2264，而进程 CPU 占用只有 5%（全在等，不在算）。
//   这正是 A/B 对照必须打开开关的原因 —— 否则量到的是 Nagle 的代价，不是系统调用的开销。
//
// 注意：`writev` 把头和体合成一次系统调用，本身就绕开了 Nagle；
// 但任何"分片写"的路径（大响应要多次写、以后加 chunked）都会踩，所以仍然要在连接上设死。
void setTcpNoDelay(int fd) {
    const int optval = 1;
    ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &optval, sizeof(optval));
}

}  // namespace

Acceptor::Acceptor(EventLoop* loop, const struct sockaddr_in& listenAddr, bool reusePort)
    : loop_(loop),
      listenFd_(createNonblockingSocket()),
      acceptChannel_(std::make_unique<Channel>(loop, listenFd_)),
      listening_(false) {
    setReuseAddr(listenFd_, true);
    setReusePort(listenFd_, reusePort);

    const int ret = ::bind(listenFd_, reinterpret_cast<const struct sockaddr*>(&listenAddr),
                           sizeof(listenAddr));
    if (ret < 0) {
        LOG_ERROR("bind failed: " << std::strerror(errno));
        std::abort();
    }

    acceptChannel_->setReadCallback([this]() { handleRead(); });
}

Acceptor::~Acceptor() {
    acceptChannel_->disableAll();
    acceptChannel_->remove();
    ::close(listenFd_);
}

void Acceptor::listen() {
    loop_->assertInLoopThread();
    listening_ = true;
    if (::listen(listenFd_, SOMAXCONN) < 0) {
        LOG_ERROR("listen failed: " << std::strerror(errno));
        std::abort();
    }
    acceptChannel_->enableReading();
}

void Acceptor::handleRead() {
    struct sockaddr_in peerAddr;
    socklen_t addrlen = sizeof(peerAddr);

    // accept4：一次调用拿到连接 fd + 非阻塞 + CLOEXEC
    const int connfd = ::accept4(listenFd_, reinterpret_cast<struct sockaddr*>(&peerAddr), &addrlen,
                                 SOCK_NONBLOCK | SOCK_CLOEXEC);
    if (connfd >= 0) {
        setTcpNoDelay(connfd);   // 见上方注释：不设会让"分片写"吃到 40ms 的 delayed-ACK 惩罚
        if (newConnectionCallback_) {
            newConnectionCallback_(connfd, peerAddr);
        } else {
            ::close(connfd);
        }
    } else {
        LOG_ERROR("accept failed: " << std::strerror(errno));
        // TODO：处理 EMFILE（fd 耗尽）等异常场景
    }
}
