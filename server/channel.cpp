#include "channel.hpp"

#include <sys/epoll.h>

#include "event_loop.hpp"

Channel::Channel(EventLoop* loop, int fd)
    : loop_(loop), fd_(fd), events_(0), revents_(0), index_(-1) {}

Channel::~Channel() {
    // 兜底：若销毁时仍注册在 epoll 中（index_ == kAdded，约定取值为 1），
    // 必须先摘除，否则 epoll 里会留下悬垂的 Channel*；
    // 已摘除的情况就直接跳过，避免在 EventLoop 析构过程中回调它。
    if (index_ == 1) {
        remove();
    }
}

void Channel::update() {
    loop_->updateChannel(this);
}

void Channel::remove() {
    loop_->removeChannel(this);
}

// 事件分发：注意顺序 —— 先处理异常，再处理读，最后处理写。
void Channel::handleEvent() {
    if ((revents_ & EPOLLHUP) && !(revents_ & EPOLLIN)) {
        if (closeCallback_) closeCallback_();
    }
    if (revents_ & EPOLLERR) {
        if (errorCallback_) errorCallback_();
    }
    if (revents_ & (EPOLLIN | EPOLLPRI | EPOLLRDHUP)) {
        if (readCallback_) readCallback_();
    }
    if (revents_ & EPOLLOUT) {
        if (writeCallback_) writeCallback_();
    }
}
