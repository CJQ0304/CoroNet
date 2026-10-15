#include "event_loop_thread_pool.hpp"

#include <cstdio>

#include "event_loop.hpp"

EventLoopThreadPool::EventLoopThreadPool(EventLoop* baseLoop, std::string name)
    : baseLoop_(baseLoop),
      name_(std::move(name)),
      started_(false),
      numThreads_(0),
      next_(0) {}

EventLoopThreadPool::~EventLoopThreadPool() {
    // 无需显式 quit：threads_ 里的 EventLoopThread 析构时会自己 quit + join。
    // （loops_ 只是裸指针，不由本类拥有）
}

void EventLoopThreadPool::start(const EventLoopThread::ThreadInitCallback& cb) {
    baseLoop_->assertInLoopThread();
    started_ = true;

    for (int i = 0; i < numThreads_; ++i) {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "%s-%d", name_.c_str(), i);
        auto thread = std::make_unique<EventLoopThread>(buf, cb);
        loops_.push_back(thread->startLoop());  // 先拿 loop 指针（会阻塞到就绪）
        threads_.push_back(std::move(thread));
    }
    LOG_INFO("EventLoopThreadPool [" << name_ << "] started, threads=" << numThreads_);

    // 线程数为 0：退化为单线程，回调在 baseLoop 上执行
    if (numThreads_ == 0 && cb) {
        cb(baseLoop_);
    }
}

EventLoop* EventLoopThreadPool::getNextLoop() {
    baseLoop_->assertInLoopThread();
    EventLoop* loop = baseLoop_;
    if (!loops_.empty()) {
        loop = loops_[next_];
        next_ = (next_ + 1) % loops_.size();
    }
    return loop;
}
