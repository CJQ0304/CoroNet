#pragma once

// ============================================================================
//  sleep.hpp —— `co_await sleepFor(loop, 200ms)` 的实现
//
//  这是理解 Awaiter "三件套" 最好的样本：
//    await_ready()   : 能不能不挂起、直接往下走？（时长<=0 就无需挂起）
//    await_suspend() : 挂起时做什么？（把定时器注册进事件循环，到点 resume 本协程）
//    await_resume()  : 被唤醒后返回什么？（sleep 无返回值）
//
//  ⚠️ 生命周期提醒：定时器回调里保存了协程句柄，所以必须保证协程不会在
//     定时器到点前被销毁。本项目里等待中的协程由连接持有，连接关闭时会唤醒
//     挂起的操作并撤销其定时器（见 tcp_connection.hpp 的 CoroOpState），
//     这个边界由「取消」机制兜住。
// ============================================================================

#include <chrono>
#include <coroutine>

#include "event_loop.hpp"

struct SleepAwaiter {
    EventLoop* loop;
    std::chrono::steady_clock::duration duration;

    bool await_ready() const noexcept {
        return duration <= std::chrono::steady_clock::duration::zero();
    }

    void await_suspend(std::coroutine_handle<> handle) const {
        const double seconds = std::chrono::duration<double>(duration).count();
        // 到点后由事件循环在 loop 线程 resume 本协程
        loop->runAfter(seconds, [handle]() { handle.resume(); });
    }

    void await_resume() const noexcept {}
};

inline SleepAwaiter sleepFor(EventLoop* loop,
                             std::chrono::steady_clock::duration d) noexcept {
    return SleepAwaiter{loop, d};
}
