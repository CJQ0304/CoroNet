#include "poller.hpp"

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <unistd.h>

#include "channel.hpp"
#include "event_loop.hpp"

EpollPoller::EpollPoller(EventLoop* loop)
    : ownerLoop_(loop),
      epollfd_(::epoll_create1(EPOLL_CLOEXEC)),
      events_(kInitEventListSize) {
    if (epollfd_ < 0) {
        LOG_ERROR("epoll_create1 failed: " << std::strerror(errno));
        std::abort();
    }
}

EpollPoller::~EpollPoller() {
    ::close(epollfd_);
}

void EpollPoller::poll(int timeoutMs, ChannelList* activeChannels) {
    const int numEvents = ::epoll_wait(epollfd_, events_.data(),
                                       static_cast<int>(events_.size()), timeoutMs);
    const int savedErrno = errno;

    if (numEvents > 0) {
        fillActiveChannels(numEvents, activeChannels);
        // 事件数量顶到缓冲区上限，说明可能还有更多，下次扩容
        if (static_cast<size_t>(numEvents) == events_.size()) {
            events_.resize(events_.size() * 2);
        }
    } else if (numEvents == 0) {
        // 超时，无事件，正常返回
    } else {
        if (savedErrno != EINTR) {
            errno = savedErrno;
            LOG_ERROR("epoll_wait failed: " << std::strerror(errno));
        }
    }
}

void EpollPoller::fillActiveChannels(int numEvents, ChannelList* activeChannels) const {
    for (int i = 0; i < numEvents; ++i) {
        auto* channel = static_cast<Channel*>(events_[i].data.ptr);
        channel->set_revents(events_[i].events);
        activeChannels->push_back(channel);
    }
}

void EpollPoller::updateChannel(Channel* channel) {
    const int index = channel->index();
    if (index == kNew || index == kDeleted) {
        channel->set_index(kAdded);
        update(EPOLL_CTL_ADD, channel);
    } else {
        if (channel->isNoneEvent()) {
            update(EPOLL_CTL_DEL, channel);
            channel->set_index(kDeleted);
        } else {
            update(EPOLL_CTL_MOD, channel);
        }
    }
}

void EpollPoller::removeChannel(Channel* channel) {
    const int index = channel->index();
    if (index == kAdded) {
        update(EPOLL_CTL_DEL, channel);
    }
    channel->set_index(kNew);
}

void EpollPoller::update(int operation, Channel* channel) {
    struct epoll_event event;
    std::memset(&event, 0, sizeof(event));
    event.events = static_cast<uint32_t>(channel->events());
    event.data.ptr = channel;

    const int fd = channel->fd();
    if (::epoll_ctl(epollfd_, operation, fd, &event) < 0) {
        if (operation == EPOLL_CTL_DEL) {
            LOG_ERROR("epoll_ctl DEL failed fd=" << fd << " : " << std::strerror(errno));
        } else {
            LOG_ERROR("epoll_ctl ADD/MOD failed fd=" << fd << " : " << std::strerror(errno));
            std::abort();
        }
    }
}
