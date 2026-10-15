#pragma once

// ============================================================================
//  EventLoopThread —— 让一个 EventLoop 跑在自己的线程里
//
//  实现要点：
//    - startLoop() 启动线程后，**阻塞等待** EventLoop 构造完成再返回它的指针，
//      否则调用方可能拿到一个还没初始化的 loop（用条件变量 + loop_ 判空实现）；
//    - 析构时先 quit 再 join，保证线程干净退出。
// ============================================================================

#include <condition_variable>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

#include "common.hpp"

class EventLoop;

class EventLoopThread : NonCopyable {
public:
    using ThreadInitCallback = std::function<void(EventLoop*)>;

    explicit EventLoopThread(std::string name,
                             ThreadInitCallback cb = ThreadInitCallback());
    ~EventLoopThread();

    // 启动线程并返回该线程内的 EventLoop 指针（阻塞直到就绪）
    EventLoop* startLoop();

private:
    void threadFunc();

    EventLoop* loop_;
    std::mutex mutex_;
    std::condition_variable cond_;
    std::thread thread_;
    const std::string name_;
    ThreadInitCallback callback_;
};
