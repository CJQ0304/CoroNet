#pragma once

// ============================================================================
//  timer.hpp —— 时间类型与定时器对象
//
//  为什么用 steady_clock 而不是 system_clock？
//    steady_clock 是"单调时钟"，只增不减，不受系统时间被改（NTP 校时、
//    用户改表）影响；定时器最怕的就是"时间突然倒退"导致定时器永不触发。
//
//  Timer 是"一个定时任务"：什么时刻触发、是否重复、触发时跑什么。
//  真正的调度逻辑在 TimerQueue（timer_queue.hpp）。
// ============================================================================

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <utility>

#include "common.hpp"

using Clock = std::chrono::steady_clock;
using Timestamp = Clock::time_point;

inline Timestamp nowTimestamp() { return Clock::now(); }

// 在 t 基础上加 seconds 秒（支持小数）
inline Timestamp addSeconds(Timestamp t, double seconds) {
    return t + std::chrono::duration_cast<Clock::duration>(
                   std::chrono::duration<double>(seconds));
}

class Timer : NonCopyable {
public:
    using TimerCallback = std::function<void()>;

    // interval > 0 表示重复定时器（每隔 interval 秒再来一次）
    Timer(TimerCallback cb, Timestamp when, double interval)
        : callback_(std::move(cb)),
          expiration_(when),
          interval_(interval),
          repeat_(interval > 0.0),
          sequence_(++s_numCreated_) {}

    void run() const { callback_(); }

    Timestamp expiration() const { return expiration_; }
    bool repeat() const { return repeat_; }
    uint64_t sequence() const { return sequence_; }

    // 重复定时器触发后，把下次到期时间往后推一个周期
    void restart(Timestamp nowT) {
        if (repeat_) {
            expiration_ = addSeconds(nowT, interval_);
        }
    }

private:
    const TimerCallback callback_;   // 触发时执行的动作
    Timestamp expiration_;           // 到期时间
    const double interval_;          // 重复周期（秒），0 表示一次性
    const bool repeat_;
    const uint64_t sequence_;        // 全局唯一序号（用于调试/未来取消）

    static std::atomic<uint64_t> s_numCreated_;
};
