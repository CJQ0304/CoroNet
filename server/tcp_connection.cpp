#include "tcp_connection.hpp"

#include <cerrno>
#include <cstring>
#include <sys/socket.h>
#include <sys/uio.h>   // writev / struct iovec
#include <thread>
#include <unistd.h>
#include <utility>

#include "channel.hpp"
#include "event_loop.hpp"

// ===========================================================================
//  协程 Awaiter：读
// ===========================================================================

bool TcpConnection::ReadAwaiter::await_ready() const noexcept {
    // 命中任一条件都不必挂起：
    //   1) 输入缓冲已有数据；
    //   2) 连接已不可用 —— 关键！否则"连接关闭时协程正挂起在别处"会一路走到这里
    //      然后永远挂起（协程泄漏）。
    return conn_->inputBuffer_->readableBytes() > 0 || conn_->state_ != kConnected;
}

void TcpConnection::ReadAwaiter::await_suspend(std::coroutine_handle<> h) {
    op_ = std::make_shared<CoroOpState>();
    op_->handle = h;
    conn_->startReadOp(op_, timeoutSeconds_);
}

std::string TcpConnection::ReadAwaiter::await_resume() {
    if (op_) {
        if (op_->cancelled) throw OperationCancelled{};
        if (op_->timedOut) throw OperationTimeout{};
    }
    // 正常完成：取走全部可读数据（空串 = EOF）
    return conn_->inputBuffer_->retrieveAllAsString();
}

// ===========================================================================
//  协程 Awaiter：写
// ===========================================================================

void TcpConnection::WriteAwaiter::await_suspend(std::coroutine_handle<> h) {
    op_ = std::make_shared<CoroOpState>();
    op_->handle = h;
    conn_->startWriteOp(op_, std::move(data_), timeoutSeconds_);
}

void TcpConnection::WriteAwaiter::await_resume() {
    if (op_) {
        if (op_->cancelled) throw OperationCancelled{};
        if (op_->timedOut) throw OperationTimeout{};
    }
}

// ===========================================================================
//  协程 Awaiter：聚集写（writev）
// ===========================================================================

void TcpConnection::WriteSegmentsAwaiter::await_suspend(std::coroutine_handle<> h) {
    op_ = std::make_shared<CoroOpState>();
    op_->handle = h;
    // segs_ 被 move 进 startWriteSegmentsOp -> sendSegmentsInLoop。
    // 未写完的部分会**拷贝**进 outputBuffer_，所以本 Awaiter 销毁后
    // 段内容的生命周期不再被依赖 —— 调用方的临时 std::string 可以安全释放。
    conn_->startWriteSegmentsOp(op_, std::move(segs_), timeoutSeconds_);
}

void TcpConnection::WriteSegmentsAwaiter::await_resume() {
    if (op_) {
        if (op_->cancelled) throw OperationCancelled{};
        if (op_->timedOut) throw OperationTimeout{};
    }
}

// ===========================================================================
//  读写操作的启动与裁决
// ===========================================================================

void TcpConnection::startReadOp(const CoroOpStatePtr& op, double timeoutSeconds) {
    loop_->assertInLoopThread();
    // readOp_ 就位后，handleRead 拿到数据时会调用 finishReadOp 唤醒本协程
    readOp_ = op;

    if (timeoutSeconds > 0) {
        TcpConnection* self = this;
        // 定时器捕获 op（shared_ptr）——即使协程先结束，op 依旧存活，晚到的定时器安全
        loop_->runAfter(timeoutSeconds, [self, op]() {
            if (op->done) return;                  // 已完成，绝不再碰 self
            self->finishReadOp(op, false, true);   // 此时协程仍挂起 => self 必然有效
        });
    }
}

void TcpConnection::startWriteOp(const CoroOpStatePtr& op, std::string data,
                                 double timeoutSeconds) {
    loop_->assertInLoopThread();
    writeOp_ = op;

    if (state_ != kConnected) {
        // 连接已不可写，直接裁决为"取消"
        finishWriteOp(op, true, false);
        return;
    }

    outputBuffer_->append(data.data(), data.size());

    // 快速路径：当前没有等待可写事件，说明内核缓冲区大概率有空间，直接写
    if (!channel_->isWriting()) {
        flushOutput();
    }

    if (outputBuffer_->readableBytes() == 0) {
        finishWriteOp(op, false, false);   // 一次写完
        return;
    }

    // 还有剩余：注册 EPOLLOUT，等 handleWrite 写完后再裁决
    channel_->enableWriting();

    if (timeoutSeconds > 0) {
        TcpConnection* self = this;
        loop_->runAfter(timeoutSeconds, [self, op]() {
            if (op->done) return;
            self->finishWriteOp(op, false, true);
        });
    }
}

// ---------------------------------------------------------------------------
//  聚集写：一次 writev 发多段
//
//  segs 只可能来自本连接的 IO 线程内的协程调用，所以不需要跨线程回投
//  （与 TcpConnection::send 不同，那个是给外部线程用的）。
// ---------------------------------------------------------------------------
void TcpConnection::sendSegmentsInLoop(const std::vector<std::string>& segs) {
    // 快路径的前提：内核里**没有比这次更早的待发数据**。
    // 若 outputBuffer_ 已有残留、或已注册 EPOLLOUT，说明队列里排着更早的字节，
    // 必须按顺序追加到队尾 —— writev 只能保证"这一次调用内部"的有序，
    // 不能让新数据插到旧数据前面（那会让响应乱序）。
    const bool canWritev = !channel_->isWriting() && outputBuffer_->readableBytes() == 0;

    std::vector<struct iovec> iov;
    if (canWritev) {
        iov.reserve(segs.size());
        for (const std::string& s : segs) {
            if (!s.empty()) {
                // writev 不会修改 iov_base 指向的内容，这里的 const_cast 只是
                // 因为 iovec 的字段类型被定义成了 void*（C 语言的遗留设计）。
                iov.push_back({const_cast<char*>(s.data()), s.size()});
            }
        }
        // 段数上限（Linux IOV_MAX = 1024）。本项目只传"头 + 体"两段，
        // 这条只是防御：真超了就退化成"整批进队列"，语义仍然正确。
        if (iov.size() > 1024) {
            iov.clear();
        }
    }

    bool written = false;
    if (!iov.empty()) {
        ssize_t n = -1;
#ifdef CORONET_AB_NO_WRITEV
        // ── A/B 对照组：把"一次 writev 发多段"退化回"逐段各一次 write" ──
        // 这不是废弃代码，而是**量化 writev 收益**的对照组：同一份源码、
        // 同一条报文、同一段后续逻辑（部分写入队 + 注册 EPOLLOUT），
        // 唯一的变量就是"每个响应几次写系统调用"。
        // 用 strace -e trace=write,writev 数次数除以完成请求数，即得
        // 确定性的对比（1 次 vs 2 次），不依赖 QPS 这种噪声大的指标。
        // 打开方式：cmake -DCORONET_BUILD_AB_DEGRADE=ON -> http_server_nowritev
        n = 0;
        for (const struct iovec& v : iov) {
            const ssize_t w = ::write(channel_->fd(), v.iov_base, v.iov_len);
            if (w < 0) {
                // 一段都没写出去时，把 errno 原样带出去（让下面的 errno 判断生效）；
                // 已经写出去了一部分的话，正常走"部分写"分支，剩余段落入队。
                if (n == 0) {
                    n = w;
                }
                break;
            }
            n += w;
            if (static_cast<size_t>(w) < v.iov_len) {
                break;   // 内核发送缓冲满，剩下的段整体入队
            }
        }
#else
        n = ::writev(channel_->fd(), iov.data(), static_cast<int>(iov.size()));
#endif
        if (n >= 0) {
            // 内核可能只写出去一部分（发送缓冲区满）。要精确算出
            // "写到第几段的第几个字节"，把剩余部分按字节位置落回 outputBuffer_。
            size_t done = static_cast<size_t>(n);
            size_t idx = 0;
            while (idx < iov.size() && done >= iov[idx].iov_len) {
                done -= iov[idx].iov_len;
                ++idx;
            }
            for (; idx < iov.size(); ++idx) {
                const char* base = static_cast<const char*>(iov[idx].iov_base) + done;
                outputBuffer_->append(base, iov[idx].iov_len - done);
                done = 0;   // 后面的段都是整段没发出去
            }
            written = true;
        } else if (errno != EWOULDBLOCK && errno != EAGAIN) {
            LOG_ERROR("sendSegmentsInLoop error [" << name_ << "]: " << std::strerror(errno));
            // 真错误也不在这里改连接状态：交给 handleWrite / 连接关闭去收场，
            // 避免在协程的调用栈上做生命周期决策。
        }
    }

    // 没走成 writev 的三种情况（队列里有更早数据 / 段数超限 / 内核缓冲满 / 出错），
    // 统统原样入队 —— 语义等价于"分成多次 write"，顺序正确。
    if (!written) {
        for (const std::string& s : segs) {
            outputBuffer_->append(s);
        }
    }

    // 还有没发完的 -> 注册 EPOLLOUT，等内核可写时由 handleWrite 继续
    if (outputBuffer_->readableBytes() > 0 && !channel_->isWriting()) {
        channel_->enableWriting();
    }
}

void TcpConnection::startWriteSegmentsOp(const CoroOpStatePtr& op, std::vector<std::string> segs,
                                         double timeoutSeconds) {
    loop_->assertInLoopThread();
    writeOp_ = op;

    if (state_ != kConnected) {
        finishWriteOp(op, true, false);   // 连接已不可写，直接裁决为"取消"
        return;
    }

    sendSegmentsInLoop(segs);

    if (outputBuffer_->readableBytes() == 0) {
        // 常见情况（响应完整写进内核缓冲）：一次 writev 就结束，没必要挂起
        finishWriteOp(op, false, false);
        return;
    }

    // 有残留：sendSegmentsInLoop 已注册 EPOLLOUT，等 handleWrite 写完后再裁决
    if (timeoutSeconds > 0) {
        TcpConnection* self = this;
        loop_->runAfter(timeoutSeconds, [self, op]() {
            if (op->done) return;
            self->finishWriteOp(op, false, true);
        });
    }
}

void TcpConnection::finishReadOp(CoroOpStatePtr op, bool cancelled, bool timedOut) {
    if (!op || op->done) return;   // 谁先到谁裁决，后到者安静退出
    op->done = true;
    op->cancelled = cancelled;
    op->timedOut = timedOut;
    if (readOp_ == op) {
        readOp_ = nullptr;
    }
    const std::coroutine_handle<> h = op->handle;
    // 延迟到本轮事件处理结束后再 resume，避免在 Channel::handleEvent() 内重入
    loop_->queueInLoop([h]() { h.resume(); });
}

void TcpConnection::finishWriteOp(CoroOpStatePtr op, bool cancelled, bool timedOut) {
    if (!op || op->done) return;
    op->done = true;
    op->cancelled = cancelled;
    op->timedOut = timedOut;
    if (writeOp_ == op) {
        writeOp_ = nullptr;
    }
    const std::coroutine_handle<> h = op->handle;
    loop_->queueInLoop([h]() { h.resume(); });
}

// ===========================================================================
//  生命周期
// ===========================================================================

TcpConnection::TcpConnection(EventLoop* loop, std::string name, int sockfd,
                             const struct sockaddr_in& localAddr, const struct sockaddr_in& peerAddr)
    : loop_(loop),
      name_(std::move(name)),
      state_(kConnecting),
      channel_(std::make_unique<Channel>(loop, sockfd)),
      inputBuffer_(std::make_unique<Buffer>()),
      outputBuffer_(std::make_unique<Buffer>()) {
    (void)localAddr;
    (void)peerAddr;

    channel_->setReadCallback([this]() { handleRead(); });
    channel_->setWriteCallback([this]() { handleWrite(); });
    channel_->setCloseCallback([this]() { handleClose(); });
    channel_->setErrorCallback([this]() { handleError(); });

    LOG_INFO("TcpConnection ctor [" << name_ << "] fd=" << sockfd);
}

TcpConnection::~TcpConnection() {
    LOG_INFO("TcpConnection dtor [" << name_ << "]");
}

void TcpConnection::connectEstablished() {
    loop_->assertInLoopThread();
    setState(kConnected);
    channel_->enableReading();
    // 这里打印的是"连接最终落在哪个 IO 线程"，用于验证多线程分发
    LOG_INFO("TcpConnection established [" << name_ << "] tid=" << std::this_thread::get_id());
    if (connectionCallback_) {
        connectionCallback_(shared_from_this());
    }
}

void TcpConnection::connectDestroyed() {
    loop_->assertInLoopThread();
    if (state_ == kConnected) {
        setState(kDisconnected);
        channel_->disableAll();
        if (connectionCallback_) {
            connectionCallback_(shared_from_this());
        }
    }
    // 兜底：唤醒任何仍挂起的协程（正常关闭路径 handleClose 已处理，
    // 但服务器析构等路径会直接走到这里），避免句柄悬垂与协程泄漏。
    if (readOp_) finishReadOp(readOp_, true, false);
    if (writeOp_) finishWriteOp(writeOp_, true, false);

    channel_->remove();
}

// ===========================================================================
//  读
// ===========================================================================

void TcpConnection::handleRead() {
    int savedErrno = 0;
    const ssize_t n = inputBuffer_->readFd(channel_->fd(), &savedErrno);
    if (n > 0) {
        if (readOp_) {
            // 有协程 co_await asyncRead()，把数据交给它（而不是走回调）
            finishReadOp(readOp_, false, false);
        } else if (messageCallback_) {
            messageCallback_(shared_from_this(), inputBuffer_.get());
        }
    } else if (n == 0) {
        handleClose();  // 对端关闭
    } else {
        errno = savedErrno;
        LOG_ERROR("read error [" << name_ << "]: " << std::strerror(errno));
        handleError();
    }
}

// ===========================================================================
//  写
// ===========================================================================

void TcpConnection::handleWrite() {
    if (!channel_->isWriting()) {
        return;
    }
    flushOutput();
    if (outputBuffer_->readableBytes() == 0) {
        channel_->disableWriting();
        if (state_ == kDisconnecting) {
            shutdownInLoop();
        }
        // 输出缓冲清空 -> 裁决挂起的写操作（正常完成）
        if (writeOp_) {
            finishWriteOp(writeOp_, false, false);
        }
    }
}

void TcpConnection::flushOutput() {
    if (outputBuffer_->readableBytes() == 0) {
        return;
    }
    // 写路径也收拢进 Buffer（与 readFd 对称），writeFd 内部已完成
    // retrieve，这里只需区分"内核缓冲满（等 EPOLLOUT）"与"真写出错"。
    int savedErrno = 0;
    const ssize_t n = outputBuffer_->writeFd(channel_->fd(), &savedErrno);
    if (n < 0 && savedErrno != EWOULDBLOCK && savedErrno != EAGAIN) {
        errno = savedErrno;
        LOG_ERROR("write error [" << name_ << "]: " << std::strerror(errno));
    }
}

// ===========================================================================
//  关闭 / 错误
// ===========================================================================

void TcpConnection::handleClose() {
    loop_->assertInLoopThread();
    LOG_INFO("connection closed [" << name_ << "]");
    setState(kDisconnected);
    channel_->disableAll();

    // 关键：唤醒挂起的协程，否则它们永远不会被恢复（泄漏）。
    //  - 读：裁决为"正常完成"，await_resume 返回空串 = EOF，协程据此退出；
    //  - 写：裁决为"取消"（对端已走，写不完了）。
    if (readOp_) finishReadOp(readOp_, false, false);
    if (writeOp_) finishWriteOp(writeOp_, true, false);

    // guardThis 保证回调期间本对象不被销毁；
    // 真正的销毁交给 TcpServer 通过 queueInLoop 延迟执行。
    TcpConnectionPtr guardThis = shared_from_this();
    if (connectionCallback_) connectionCallback_(guardThis);
    if (closeCallback_) closeCallback_(guardThis);
}

void TcpConnection::handleError() {
    int err = 0;
    socklen_t optlen = sizeof(err);
    ::getsockopt(channel_->fd(), SOL_SOCKET, SO_ERROR, &err, &optlen);
    LOG_ERROR("socket error [" << name_ << "]: " << std::strerror(err));
}

// ===========================================================================
//  回调式 send / shutdown（非协程接口，协程模式下不再使用）
// ===========================================================================

// 线程安全：可能在别的线程被调用，必须把实际操作送回本连接所属的 IO 线程。
// 捕获 self（shared_ptr）保证任务执行前连接不被销毁。
void TcpConnection::send(const std::string& message) {
    if (loop_->isInLoopThread()) {
        if (state_ == kConnected) {
            sendInLoop(message);
        }
    } else {
        TcpConnectionPtr self = shared_from_this();
        loop_->runInLoop([self, message]() {
            if (self->state_ == kConnected) {
                self->sendInLoop(message);
            }
        });
    }
}

// 先尝试直接 write；写不完的进 outputBuffer_，并注册 EPOLLOUT 等内核可写。
void TcpConnection::sendInLoop(const std::string& message) {
    ssize_t nwrote = 0;
    size_t remaining = message.size();

    if (!channel_->isWriting() && outputBuffer_->readableBytes() == 0) {
        nwrote = ::write(channel_->fd(), message.data(), message.size());
        if (nwrote >= 0) {
            remaining = message.size() - static_cast<size_t>(nwrote);
        } else {
            nwrote = 0;
            if (errno != EWOULDBLOCK) {
                LOG_ERROR("send error [" << name_ << "]: " << std::strerror(errno));
            }
        }
    }

    if (remaining > 0) {
        outputBuffer_->append(message.data() + nwrote, remaining);
        if (!channel_->isWriting()) {
            channel_->enableWriting();
        }
    }
}

void TcpConnection::shutdown() {
    if (loop_->isInLoopThread()) {
        if (state_ == kConnected) {
            setState(kDisconnecting);
            shutdownInLoop();
        }
    } else {
        TcpConnectionPtr self = shared_from_this();
        loop_->runInLoop([self]() {
            if (self->state_ == kConnected) {
                self->setState(kDisconnecting);
                self->shutdownInLoop();
            }
        });
    }
}

void TcpConnection::shutdownInLoop() {
    if (!channel_->isWriting()) {
        ::shutdown(channel_->fd(), SHUT_WR);
    }
}
