#pragma once

// ============================================================================
//  TimerQueue —— 定时器队列（基于 Linux timerfd）
//
//  设计要点：
//    1. 用一个 timerfd 把"时间"变成"fd 可读事件"，从而**无缝融入 epoll**——
//       事件循环不需要为定时器单独算 poll 超时，逻辑统一。
//    2. timers_ 是 std::set<pair<Timestamp, Timer*>>：按到期时间有序，
//       于是 timers_.begin() 就是"最近要触发的那个"，O(log n) 增删。
//    3. 每次插入若发现新定时器最早到期，就用 timerfd_settime 重设内核定时。
//    4. 本类**只在 loop 线程访问**（EventLoop 的 runAt 会保证这一点），
//       因此内部无锁，避免无谓的互斥开销。
// ============================================================================

#include <set>
#include <utility>
#include <vector>

#include "channel.hpp"
#include "common.hpp"
#include "timer.hpp"

class EventLoop;

class TimerQueue : NonCopyable {
public:
    explicit TimerQueue(EventLoop* loop);
    ~TimerQueue();

    // 把 timerfd 注册进 epoll（由 EventLoop 构造完成后调用，
    // 避免在 EventLoop 成员尚未初始化时回调它）
    void start();

    // 添加定时器；interval > 0 表示重复。仅在 loop 线程调用。
    void addTimer(Timer::TimerCallback cb, Timestamp when, double interval);

private:
    using Entry = std::pair<Timestamp, Timer*>;
    using TimerList = std::set<Entry>;

    void handleRead();                                  // timerfd 可读 = 有定时器到期
    std::vector<Entry> getExpired(Timestamp nowT);      // 取出所有已到期的
    void reset(const std::vector<Entry>& expired, Timestamp nowT);  // 重复定时器重排
    bool insert(Timer* timer);                          // 返回"是否成为最早的那个"

    EventLoop* loop_;
    const int timerfd_;
    Channel timerfdChannel_;
    TimerList timers_;
};
