#include "event_loop_thread.hpp"

#include "event_loop.hpp"

EventLoopThread::EventLoopThread(std::string name, ThreadInitCallback cb)
    : loop_(nullptr), name_(std::move(name)), callback_(std::move(cb)) {}

EventLoopThread::~EventLoopThread() {
    if (loop_ != nullptr) {
        loop_->quit();  // 让 loop() 退出；quit 内部会 wakeup
    }
    if (thread_.joinable()) {
        thread_.join();
    }
}

EventLoop* EventLoopThread::startLoop() {
    thread_ = std::thread([this]() { threadFunc(); });

    EventLoop* loop = nullptr;
    {
        std::unique_lock<std::mutex> lock(mutex_);
        // 等 loop_ 被 threadFunc 填上，确保 EventLoop 已构造完毕
        cond_.wait(lock, [this]() { return loop_ != nullptr; });
        loop = loop_;
    }
    return loop;
}

void EventLoopThread::threadFunc() {
    EventLoop loop;  // 栈上创建，生命周期 = 本线程
    if (callback_) {
        callback_(&loop);
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        loop_ = &loop;
        cond_.notify_one();
    }

    LOG_INFO("EventLoopThread [" << name_ << "] started");
    loop.loop();  // 阻塞直到 quit

    std::lock_guard<std::mutex> lock(mutex_);
    loop_ = nullptr;
    LOG_INFO("EventLoopThread [" << name_ << "] exiting");
}
