#pragma once

// ============================================================================
//  EpollPoller —— epoll 的 C++ 封装
//
//  职责很单一：
//    1. poll()：调用 epoll_wait 等事件，把就绪的 Channel 收集到 activeChannels；
//    2. updateChannel()：把 Channel 关心的事件同步到内核（ADD/MOD/DEL）。
//
//  "poller" 这个名字是抽象，将来想换成 poll/select/kqueue 只需换实现。
//  事件与 Channel 的关联方式：把 Channel* 存进 epoll_event.data.ptr，
//  epoll_wait 返回后直接取回指针 —— 零查找开销。
// ============================================================================

#include <sys/epoll.h>
#include <vector>

#include "common.hpp"

class Channel;
class EventLoop;

class EpollPoller : NonCopyable {
public:
    using ChannelList = std::vector<Channel*>;

    explicit EpollPoller(EventLoop* loop);
    ~EpollPoller();

    // 等待事件，超时时间 timeoutMs；就绪的 Channel 追加到 activeChannels
    void poll(int timeoutMs, ChannelList* activeChannels);

    // 根据 Channel 当前关心的事件，决定 ADD / MOD / DEL
    void updateChannel(Channel* channel);
    void removeChannel(Channel* channel);

private:
    static constexpr int kInitEventListSize = 16;

    // Channel::index_ 的三个取值
    static constexpr int kNew = -1;
    static constexpr int kAdded = 1;
    static constexpr int kDeleted = 2;

    void fillActiveChannels(int numEvents, ChannelList* activeChannels) const;
    void update(int operation, Channel* channel);

    EventLoop* ownerLoop_;
    int epollfd_;
    std::vector<epoll_event> events_;  // epoll_wait 的输出缓冲区
};
