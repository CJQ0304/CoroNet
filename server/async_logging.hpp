#pragma once

// ============================================================================
//  async_logging.hpp —— 异步日志的「双缓冲」核心
//
//  一句话概括：**前端只往内存里 memcpy，后端线程定时把整块内容落盘。**
//
//  数据流：
//      多个业务线程                       日志后端线程（唯一 I/O 者）
//      ┌──────────────┐                   ┌──────────────────────────┐
//      │ append(data) │                   │ wait_for(flushInterval)  │
//      │   ↓ 加锁      │                   │   ↓ 被唤醒/超时           │
//      │ currentBuffer│ ──满了/停下时──►  │ 交换 buffers_ 与本地列表  │
//      │  memcpy 追加  │                   │   ↓ 锁外 fwrite + flush   │
//      └──────────────┘                   └──────────────────────────┘
//
//  三个关键设计：
//
//  1. **缓冲区交换（双缓冲）而不是队列**：前端攒满一整块（1MB）才把所有权
//     交给后端，后端**一次性**拿到整块去写。这样：
//       - 加锁时间 = 一次 memcpy 的时间，锁竞争极短；
//       - 后端写盘时**不持锁**，前端完全不受影响。
//     这正是 muduo 日志库的核心思路，比"每条日志入一个队列"高效得多
//     （后者要为每个节点分配内存、每次都要唤醒后端）。
//
//  2. **缓冲区回收复用**：后端写完不 free，而是把这块 vector 还回前端复用
//     （clear() 保留 capacity）。稳态下几乎不再发生 malloc。
//
//  3. **前端永不阻塞在 I/O 上**：追加只可能被"另一个线程正在追加"短暂卡住，
//     绝不会因为磁盘慢而停住。代价是进程崩溃时最后一批日志可能丢失——
//     这是异步日志的固有取舍（可另配 sync 模式的关键日志，本项目不做）。
// ============================================================================

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "log_file.hpp"

namespace coronet {

class AsyncLogging {
public:
    using Buffer = std::vector<char>;
    using BufferPtr = std::unique_ptr<Buffer>;

    // 单块前端缓冲大小：越大 → 加锁/唤醒次数越少、内存占用越高
    static constexpr size_t kBufferSize = 1024 * 1024;

    AsyncLogging(std::string basename, size_t rollSize, double flushIntervalSeconds, bool console);
    ~AsyncLogging();

    AsyncLogging(const AsyncLogging&) = delete;
    AsyncLogging& operator=(const AsyncLogging&) = delete;

    void start();   // 拉起后端线程
    void stop();    // 停后端线程（退出前把残留日志刷掉）

    void append(const char* data, size_t len);   // 前端：任意线程可调
    void flush();                                // 请求后端尽快落盘

private:
    void threadFunc();

    std::mutex mutex_;
    std::condition_variable cond_;

    BufferPtr currentBuffer_;         // 前端正在写的那块
    BufferPtr nextBuffer_;            // 备用块：前端写满时立刻顶上，不必现分配
    std::vector<BufferPtr> buffers_;  // 已写满、等后端接手的块

    std::atomic<bool> running_{false};
    std::unique_ptr<std::thread> thread_;

    LogFile output_;
    const double flushInterval_;
    const bool console_;
};

}  // namespace coronet
