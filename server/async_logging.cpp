#include "async_logging.hpp"

#include <cstdio>
#include <utility>

namespace coronet {

AsyncLogging::AsyncLogging(std::string basename, size_t rollSize, double flushIntervalSeconds,
                           bool console)
    : output_(std::move(basename), rollSize),
      flushInterval_(flushIntervalSeconds),
      console_(console) {
    // 两块初始缓冲在**启动前**就准备好：前端第一次 append 绝不可能拿到空指针
    currentBuffer_ = std::make_unique<Buffer>();
    currentBuffer_->reserve(kBufferSize);
    nextBuffer_ = std::make_unique<Buffer>();
    nextBuffer_->reserve(kBufferSize);
}

AsyncLogging::~AsyncLogging() {
    stop();
}

void AsyncLogging::start() {
    if (running_.exchange(true)) {
        return;   // 已在运行
    }
    thread_ = std::make_unique<std::thread>([this]() { threadFunc(); });
}

void AsyncLogging::stop() {
    if (!running_.exchange(false)) {
        return;   // 幂等：重复调用安全
    }
    cond_.notify_all();   // 唤醒后端，让它跑完最后一轮（把残留日志刷盘）
    if (thread_ != nullptr && thread_->joinable()) {
        thread_->join();
    }
    thread_.reset();
}

// ---------------------------------------------------------------------------
//  前端：唯一的"热路径"。目标是一个 memcpy 加一次极短的加锁。
// ---------------------------------------------------------------------------
void AsyncLogging::append(const char* data, size_t len) {
    if (!running_.load(std::memory_order_relaxed)) {
        // 后端已停（进程正在退出）：直接写 stderr，至少不丢日志
        ::fwrite(data, 1, len, stderr);
        return;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    if (currentBuffer_ == nullptr) {
        return;   // 理论不可达，纯防御
    }

    if (currentBuffer_->size() + len > kBufferSize) {
        // 这一块写满了：整块交给后端，自己换一块空的接着写。
        // 注意是"转移所有权"，不是拷贝——这是双缓冲省时间的关键。
        buffers_.push_back(std::move(currentBuffer_));
        if (nextBuffer_ != nullptr) {
            currentBuffer_ = std::move(nextBuffer_);   // 备用块顶上，零分配
        } else {
            currentBuffer_ = std::make_unique<Buffer>();
            currentBuffer_->reserve(kBufferSize);
        }
        // 已经攒满一整块，立刻叫醒后端落盘（否则要等到 flushInterval 超时）
        cond_.notify_one();
    }

    currentBuffer_->insert(currentBuffer_->end(), data, data + len);
}

void AsyncLogging::flush() {
    // 不需要额外的"flush 请求"状态：后端每一轮结束都会 flush，
    // 所以只要把它叫醒，它就会把当前攒的内容全部写出去。
    cond_.notify_one();
}

// ---------------------------------------------------------------------------
//  后端线程：进程里唯一碰磁盘的地方
// ---------------------------------------------------------------------------
void AsyncLogging::threadFunc() {
    // 后端自己的两块备用缓冲（与前端那两块互相轮换）
    BufferPtr newBuffer1 = std::make_unique<Buffer>();
    BufferPtr newBuffer2 = std::make_unique<Buffer>();
    newBuffer1->reserve(kBufferSize);
    newBuffer2->reserve(kBufferSize);

    std::vector<BufferPtr> buffersToWrite;
    buffersToWrite.reserve(16);

    while (running_.load(std::memory_order_relaxed)) {
        // ---- 临界区：只做「等待 + 交换」，绝不在这里写盘 ----
        {
            std::unique_lock<std::mutex> lock(mutex_);
            cond_.wait_for(lock, std::chrono::duration<double>(flushInterval_), [this]() {
                return !buffers_.empty() || (currentBuffer_ != nullptr && !currentBuffer_->empty()) ||
                       !running_.load(std::memory_order_relaxed);
            });

            // 把前端「正在写的那块」也一并接管（它可能只装了半块日志）。
            // 前端随后拿到 newBuffer1 继续写，互不影响。
            if (currentBuffer_ != nullptr) {
                buffers_.push_back(std::move(currentBuffer_));
            }
            currentBuffer_ = std::move(newBuffer1);

            buffersToWrite.swap(buffers_);   // 双缓冲交换：一次指针级操作

            if (nextBuffer_ == nullptr && newBuffer2 != nullptr) {
                nextBuffer_ = std::move(newBuffer2);
            }
        }

        // ---- 锁外写盘：这一步再慢也影响不到业务线程 ----
        if (buffersToWrite.size() > 25) {
            // 前端产生日志的速度长期超过后端落盘速度 —— 说明后端是瓶颈
            ::fprintf(stderr, "[coronet] log backend falling behind: %zu buffers pending\n",
                      buffersToWrite.size());
        }

        for (BufferPtr& b : buffersToWrite) {
            if (b != nullptr && !b->empty()) {
                output_.append(b->data(), b->size());
                if (console_) {
                    // 镜像到终端由**后端线程**完成，所以业务线程依旧零 I/O。
                    // 调试时开、压测时关（关掉才能测出异步日志的真实开销）。
                    ::fwrite(b->data(), 1, b->size(), stderr);
                }
            }
        }
        output_.flush();

        // ---- 回收复用：把刚写完的 vector 还给上面两块备用位 ----
        if (newBuffer1 == nullptr && !buffersToWrite.empty()) {
            newBuffer1 = std::move(buffersToWrite.back());
            buffersToWrite.pop_back();
            newBuffer1->clear();   // clear 保留 capacity，下次不必再扩容
        }
        if (newBuffer2 == nullptr && !buffersToWrite.empty()) {
            newBuffer2 = std::move(buffersToWrite.back());
            buffersToWrite.pop_back();
            newBuffer2->clear();
        }
        if (newBuffer1 == nullptr) {
            newBuffer1 = std::make_unique<Buffer>();
            newBuffer1->reserve(kBufferSize);
        }
        if (newBuffer2 == nullptr) {
            newBuffer2 = std::make_unique<Buffer>();
            newBuffer2->reserve(kBufferSize);
        }
        buffersToWrite.clear();   // 其余块释放；稳态下这里通常是空的
    }

    // ---- 退出前最后一轮：把残留内容全部落盘 ----
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (currentBuffer_ != nullptr && !currentBuffer_->empty()) {
            buffers_.push_back(std::move(currentBuffer_));
        }
        for (BufferPtr& b : buffers_) {
            if (b != nullptr && !b->empty()) {
                output_.append(b->data(), b->size());
                if (console_) {
                    ::fwrite(b->data(), 1, b->size(), stderr);
                }
            }
        }
        buffers_.clear();
    }
    output_.flush();
}

}  // namespace coronet
