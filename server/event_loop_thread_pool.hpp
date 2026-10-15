#pragma once

// ============================================================================
//  EventLoopThreadPool —— 一组 IO 线程（one loop per thread）
//
//  主从 Reactor 里的"从 Reactor"池：
//    - baseLoop（主线程）负责 accept；
//    - 新连接用 getNextLoop() 轮询(Round-Robin)分配给某个 IO 线程，
//      之后该连接的所有读写都在那个线程完成 —— 这就是"单连接单线程、
//      连接内无锁"的由来。
//    - setThreadNum(0) 时退化为单线程（全部跑在 baseLoop）。
// ============================================================================

#include <memory>
#include <string>
#include <vector>

#include "common.hpp"
#include "event_loop_thread.hpp"

class EventLoop;

class EventLoopThreadPool : NonCopyable {
public:
    EventLoopThreadPool(EventLoop* baseLoop, std::string name);
    ~EventLoopThreadPool();

    void setThreadNum(int numThreads) { numThreads_ = numThreads; }

    void start(const EventLoopThread::ThreadInitCallback& cb =
                   EventLoopThread::ThreadInitCallback());

    // 轮询取下一个 IO loop；线程数为 0 时返回 baseLoop
    EventLoop* getNextLoop();

    const std::string& name() const { return name_; }

private:
    EventLoop* baseLoop_;
    const std::string name_;
    bool started_;
    int numThreads_;
    size_t next_;
    std::vector<std::unique_ptr<EventLoopThread>> threads_;
    std::vector<EventLoop*> loops_;
};
