#pragma once

// ============================================================================
//  Channel —— Reactor 的"事件单元"
//
//  一个 Channel 绑定一个 fd，负责：
//    1. 记录"我关心哪些事件"（events_，通过 enable/disable 修改）；
//    2. 事件发生时，由 EpollPoller 把"实际发生的事件"填进 revents_，
//       再由 Channel::handleEvent() 分发到对应的回调。
//
//  它是 Acceptor / TcpConnection 与 epoll 之间的桥梁：
//    上层只关心"可读时做什么"，不用碰 epoll 的细节。
//
//  使用水平触发(LT)：只要缓冲区还有数据，epoll 会持续通知，
//  因此每次 handleRead 只读一次也不会丢数据，与协程"一次 co_await 读一次"
//  的语义天然对齐（不选边缘触发的理由见 README 第七节）。
// ============================================================================

#include <functional>
#include <sys/epoll.h>

#include "common.hpp"

class EventLoop;

class Channel : NonCopyable {
public:
    using EventCallback = std::function<void()>;

    Channel(EventLoop* loop, int fd);
    ~Channel();

    // 事件分发入口：EventLoop 收到事件后调用它
    void handleEvent();

    void setReadCallback(EventCallback cb) { readCallback_ = std::move(cb); }
    void setWriteCallback(EventCallback cb) { writeCallback_ = std::move(cb); }
    void setCloseCallback(EventCallback cb) { closeCallback_ = std::move(cb); }
    void setErrorCallback(EventCallback cb) { errorCallback_ = std::move(cb); }

    int fd() const { return fd_; }
    int events() const { return events_; }
    void set_revents(int revt) { revents_ = revt; }
    bool isNoneEvent() const { return events_ == kNoneEvent; }

    void enableReading() { events_ |= kReadEvent; update(); }
    void disableReading() { events_ &= ~kReadEvent; update(); }
    void enableWriting() { events_ |= kWriteEvent; update(); }
    void disableWriting() { events_ &= ~kWriteEvent; update(); }
    void disableAll() { events_ = kNoneEvent; update(); }

    bool isWriting() const { return (events_ & kWriteEvent) != 0; }
    bool isReading() const { return (events_ & kReadEvent) != 0; }

    // index_ 由 EpollPoller 维护：-1=新, 1=已加入, 2=已删除
    int index() const { return index_; }
    void set_index(int idx) { index_ = idx; }

    EventLoop* ownerLoop() { return loop_; }

    // 把自己从 epoll 中摘掉（析构前的兜底）
    void remove();

private:
    void update();  // 通知 EventLoop 更新 epoll 关注的事件

    static constexpr int kNoneEvent = 0;
    static constexpr int kReadEvent = EPOLLIN | EPOLLPRI;
    static constexpr int kWriteEvent = EPOLLOUT;

    EventLoop* loop_;
    const int fd_;
    int events_;    // 关心的事件
    int revents_;   // poller 填进来的、实际发生的事件
    int index_;

    EventCallback readCallback_;
    EventCallback writeCallback_;
    EventCallback closeCallback_;
    EventCallback errorCallback_;
};
