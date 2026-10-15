#pragma once

// ============================================================================
//  EventLoop —— Reactor 的心脏
//
//  三样跨线程基础设施：
//    1. eventfd：一个"自唤醒管道"。别的线程想让我干活时，往 eventfd 写 8 字节，
//       epoll_wait 立即返回，而不用傻等超时；
//    2. mutex_：保护 pendingFunctors_（跨线程投递任务的队列）；
//    3. quit() 跨线程即时生效。
//
//  配合 TimerQueue：把"定时"变成"timerfd 可读事件"，
//  于是 sleep / 超时等时间相关能力，全都统一进了同一个 epoll 循环。
//
//  铁律：**一个 fd 只能由一个线程操作**。跨线程做事必须用
//  runInLoop/queueInLoop 把任务"送回"目标 loop 线程执行。
// ============================================================================

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "common.hpp"
#include "timer.hpp"  // Timestamp / Timer::TimerCallback

class Channel;
class EpollPoller;
class TimerQueue;

class EventLoop : NonCopyable {
public:
    using Functor = std::function<void()>;
    using ChannelList = std::vector<Channel*>;

    EventLoop();
    ~EventLoop();

    void loop();   // 启动事件循环（阻塞，直到 quit）
    void quit();   // 退出事件循环（可从任意线程调用）

    void updateChannel(Channel* channel);
    void removeChannel(Channel* channel);

    void runInLoop(Functor cb);
    void queueInLoop(Functor cb);

    // ---- 定时器：任一时刻执行 / 延迟执行 / 周期执行 ----
    void runAt(Timestamp when, Timer::TimerCallback cb);
    void runAfter(double delaySeconds, Timer::TimerCallback cb);
    void runEvery(double intervalSeconds, Timer::TimerCallback cb);

    bool isInLoopThread() const { return threadId_ == std::this_thread::get_id(); }
    void assertInLoopThread() const;

    // 唤醒阻塞在 epoll_wait 的本线程
    void wakeup();

private:
    void handleWakeup();
    void doPendingFunctors();

    std::unique_ptr<EpollPoller> poller_;
    int wakeupFd_;
    std::unique_ptr<Channel> wakeupChannel_;
    std::unique_ptr<TimerQueue> timerQueue_;   // 声明在 poller_ 之后 => 先于 poller_ 析构
    bool looping_;
    std::atomic<bool> quit_;
    const std::thread::id threadId_;

    std::mutex mutex_;                       // 保护 pendingFunctors_
    std::vector<Functor> pendingFunctors_;
    bool callingPendingFunctors_;            // 只在 loop 线程读写
};
