#include "event_loop.hpp"

#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <sys/eventfd.h>
#include <unistd.h>

#include "channel.hpp"
#include "poller.hpp"
#include "timer_queue.hpp"

namespace {

// eventfd：内核提供的一个"计数器管道"，天生适合做线程间唤醒。
// 写 8 字节 -> 读端变可读；读走 8 字节 -> 恢复不可读。
int createEventfd() {
    const int evtfd = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (evtfd < 0) {
        LOG_ERROR("eventfd() failed: " << std::strerror(errno));
        std::abort();
    }
    return evtfd;
}

}  // namespace

EventLoop::EventLoop()
    : poller_(std::make_unique<EpollPoller>(this)),
      wakeupFd_(createEventfd()),
      wakeupChannel_(std::make_unique<Channel>(this, wakeupFd_)),
      timerQueue_(std::make_unique<TimerQueue>(this)),
      looping_(false),
      quit_(false),
      threadId_(std::this_thread::get_id()),
      callingPendingFunctors_(false) {
    // 把 eventfd 也纳入 epoll 管理：它可读 = 有人来喊我干活
    wakeupChannel_->setReadCallback([this]() { handleWakeup(); });
    wakeupChannel_->enableReading();

    // 所有成员就绪后再启动定时器（此时 updateChannel 里读 threadId_ 才安全）
    timerQueue_->start();
}

EventLoop::~EventLoop() {
    // 先销毁定时器队列：它会从 epoll 摘掉 timerfd，并释放未触发的 Timer
    timerQueue_.reset();

    wakeupChannel_->disableAll();
    wakeupChannel_->remove();
    ::close(wakeupFd_);
}

void EventLoop::assertInLoopThread() const {
    if (!isInLoopThread()) {
        LOG_ERROR("EventLoop used from a wrong thread!");
        std::abort();
    }
}

void EventLoop::loop() {
    assertInLoopThread();
    if (looping_) {
        LOG_ERROR("EventLoop::loop() called twice");
        return;
    }
    looping_ = true;
    quit_ = false;

    ChannelList activeChannels;
    while (!quit_) {
        activeChannels.clear();
        // 10s 超时只是"空闲兜底"；有事件或有唤醒时立即返回
        poller_->poll(10000, &activeChannels);

        for (Channel* channel : activeChannels) {
            channel->handleEvent();
        }

        // 本轮事件处理完，再执行排队任务（延迟销毁、跨线程投递都靠它）
        doPendingFunctors();
    }
    looping_ = false;
}

void EventLoop::quit() {
    quit_ = true;
    // 不在本线程说明对方正阻塞在 epoll_wait，必须喊醒它
    if (!isInLoopThread()) {
        wakeup();
    }
}

void EventLoop::wakeup() {
    const uint64_t one = 1;
    const ssize_t n = ::write(wakeupFd_, &one, sizeof(one));
    (void)n;
}

void EventLoop::handleWakeup() {
    uint64_t one = 1;
    const ssize_t n = ::read(wakeupFd_, &one, sizeof(one));
    (void)n;
}

void EventLoop::updateChannel(Channel* channel) {
    assertInLoopThread();
    poller_->updateChannel(channel);
}

void EventLoop::removeChannel(Channel* channel) {
    assertInLoopThread();
    poller_->removeChannel(channel);
}

void EventLoop::runInLoop(Functor cb) {
    if (isInLoopThread()) {
        cb();  // 已在本线程，直接执行，省一次唤醒
    } else {
        queueInLoop(std::move(cb));
    }
}

void EventLoop::queueInLoop(Functor cb) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        pendingFunctors_.push_back(std::move(cb));
    }
    // 跨线程投递 -> 唤醒；
    // 或正在执行待办任务时又投递 -> 也要唤醒，保证下一轮能被处理
    if (!isInLoopThread() || callingPendingFunctors_) {
        wakeup();
    }
}

void EventLoop::doPendingFunctors() {
    std::vector<Functor> functors;
    callingPendingFunctors_ = true;
    {
        // 只锁 swap 这一下，执行回调时不持锁，避免回调里再投递导致死锁
        std::lock_guard<std::mutex> lock(mutex_);
        functors.swap(pendingFunctors_);
    }
    for (Functor& f : functors) {
        f();
    }
    callingPendingFunctors_ = false;
}

// ---------------------------------------------------------------------------
//  定时器接口
//
//  TimerQueue 内部无锁（只在 loop 线程访问），所以若从别的线程调用，
//  必须把"添加定时器"这件事用 queueInLoop 送回 loop 线程。
// ---------------------------------------------------------------------------
void EventLoop::runAt(Timestamp when, Timer::TimerCallback cb) {
    if (isInLoopThread()) {
        timerQueue_->addTimer(std::move(cb), when, 0.0);
    } else {
        queueInLoop([this, when, cb = std::move(cb)]() mutable {
            timerQueue_->addTimer(std::move(cb), when, 0.0);
        });
    }
}

void EventLoop::runAfter(double delaySeconds, Timer::TimerCallback cb) {
    runAt(addSeconds(Clock::now(), delaySeconds), std::move(cb));
}

void EventLoop::runEvery(double intervalSeconds, Timer::TimerCallback cb) {
    if (isInLoopThread()) {
        timerQueue_->addTimer(std::move(cb), addSeconds(Clock::now(), intervalSeconds),
                              intervalSeconds);
    } else {
        queueInLoop([this, intervalSeconds, cb = std::move(cb)]() mutable {
            timerQueue_->addTimer(std::move(cb),
                                  addSeconds(Clock::now(), intervalSeconds),
                                  intervalSeconds);
        });
    }
}
