#pragma once

// ============================================================================
//  coro_error.hpp —— 协程操作的状态与错误
//
//  核心问题：一个协程 `co_await asyncRead()` 后挂起，它有**三种**可能的归宿：
//    ① 正常完成（epoll 通知有数据）；
//    ② 超时（我们自己起的定时器到点）；
//    ③ 被取消（连接销毁 / 主动取消）。
//  三者可能在**同一时刻**都想唤醒它 —— 比如"数据刚好到达"和"超时定时器刚好触发"。
//  若不做保护，同一协程会被 resume 两次 → 未定义行为。
//
//  解法：把"这次挂起操作"的状态放在堆上（shared_ptr），用 done 标记做**一次性**裁决，
//  谁先把 done 置真谁就负责 resume。晚到的定时器/回调只会看到 done=true 而安静退出。
//
//  又因为状态是 shared_ptr 共享的，**即使协程先结束、晚到的定时器仍持有它**也不会悬垂。
// ============================================================================

#include <coroutine>
#include <exception>
#include <memory>

// 操作被取消（连接销毁、显式取消）
class OperationCancelled : public std::exception {
public:
    const char* what() const noexcept override { return "operation cancelled"; }
};

// 操作超时
class OperationTimeout : public std::exception {
public:
    const char* what() const noexcept override { return "operation timed out"; }
};

// 一次挂起操作的状态：连接（epoll 回调）与定时器（超时）共享。
// 仅在所属连接的 IO 线程访问，因此 done 用普通 bool 即可。
struct CoroOpState {
    std::coroutine_handle<> handle{};  // 挂起中的协程句柄
    bool done{false};                  // 是否已裁决（防重复 resume）
    bool cancelled{false};             // 归宿：被取消
    bool timedOut{false};              // 归宿：超时
};

using CoroOpStatePtr = std::shared_ptr<CoroOpState>;
