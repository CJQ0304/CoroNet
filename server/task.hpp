#pragma once

// ============================================================================
//  task.hpp —— C++20 协程的两种"返回类型"
//
//  1) Task<T>        ：惰性协程。创建后先挂起，可被 `co_await`（有返回值），
//                      也可手动 start()。用于"任务组合"。
//  2) DetachedTask   ：立即启动、结束即自毁的协程。用于 fire-and-forget，
//                      比如"每个连接起一个协程"这种顶层入口。
//
//  ── 理解 C++20 协程的三个关键点 ─────────────────────────────────
//  ① promise_type 是"协程的控制器"：编译器把协程体编译成一堆状态机分支，
//     通过 promise_type 的接口管理生命周期与返回值。
//  ② initial_suspend / final_suspend 决定协程"起步/收尾"是否挂起：
//       suspend_always → 惰性；suspend_never → 立即执行 / 结束即销毁。
//  ③ Awaiter 三件套 await_ready / await_suspend / await_resume 驱动 co_await：
//      ready 为 true 就不挂起；suspend 里拿到 handle（可挂到别处，等条件满足再 resume）。
// ============================================================================

#include <coroutine>
#include <exception>
#include <optional>
#include <utility>

#include "common.hpp"

// ===========================================================================
//  Task<T> —— 惰性、有返回值、可 co_await 的协程
// ===========================================================================
template <typename T = void>
class Task {
public:
    struct promise_type {
        std::coroutine_handle<> continuation_{};  // 父协程（谁在 co_await 我）
        std::optional<T> value_{};                // co_return 的结果
        std::exception_ptr exception_{};          // 未捕获异常

        Task get_return_object() noexcept {
            return Task{std::coroutine_handle<promise_type>::from_promise(*this)};
        }

        // 惰性：创建后先挂起，等 start() 或被 co_await
        std::suspend_always initial_suspend() noexcept { return {}; }

        // 收尾：把控制权交还给父协程；没有父协程就交还事件循环
        struct FinalAwaiter {
            bool await_ready() const noexcept { return false; }
            std::coroutine_handle<> await_suspend(
                std::coroutine_handle<promise_type> h) const noexcept {
                std::coroutine_handle<> cont = h.promise().continuation_;
                return cont ? cont : std::noop_coroutine();
            }
            void await_resume() const noexcept {}
        };
        FinalAwaiter final_suspend() noexcept { return {}; }

        template <typename U>
        void return_value(U&& v) {
            value_.emplace(std::forward<U>(v));
        }
        void unhandled_exception() noexcept { exception_ = std::current_exception(); }
    };

    using handle_type = std::coroutine_handle<promise_type>;

    Task() noexcept = default;
    explicit Task(handle_type h) noexcept : handle_(h) {}
    Task(Task&& other) noexcept : handle_(std::exchange(other.handle_, handle_type{})) {}

    Task& operator=(Task&& other) noexcept {
        if (this != &other) {
            if (handle_) handle_.destroy();
            handle_ = std::exchange(other.handle_, handle_type{});
        }
        return *this;
    }
    Task(const Task&) = delete;
    Task& operator=(const Task&) = delete;

    ~Task() {
        if (handle_) handle_.destroy();  // 谁创建谁销毁，防止执行帧泄漏
    }

    void start() {
        if (handle_ && !handle_.done()) handle_.resume();
    }
    bool done() const noexcept { return !handle_ || handle_.done(); }

    // ---- Awaiter 接口：让 `co_await someTask()` 可用 ----
    bool await_ready() const noexcept { return false; }

    std::coroutine_handle<> await_suspend(std::coroutine_handle<> caller) noexcept {
        handle_.promise().continuation_ = caller;  // 记住父协程
        return handle_;                            // 把控制权交给子协程
    }

    T await_resume() {
        if (handle_.promise().exception_) {
            std::rethrow_exception(handle_.promise().exception_);
        }
        return std::move(*handle_.promise().value_);
    }

private:
    handle_type handle_{};
};

// ---------------------------------------------------------------------------
//  Task<void> 偏特化：没有返回值，其余逻辑与通用版一致
//  （C++20 要求 promise 要么有 return_value 要么有 return_void，不能共存，
//    所以 void 必须单独写一份）
// ---------------------------------------------------------------------------
template <>
class Task<void> {
public:
    struct promise_type {
        std::coroutine_handle<> continuation_{};
        std::exception_ptr exception_;

        Task get_return_object() noexcept {
            return Task{std::coroutine_handle<promise_type>::from_promise(*this)};
        }
        std::suspend_always initial_suspend() noexcept { return {}; }

        struct FinalAwaiter {
            bool await_ready() const noexcept { return false; }
            std::coroutine_handle<> await_suspend(
                std::coroutine_handle<promise_type> h) const noexcept {
                std::coroutine_handle<> cont = h.promise().continuation_;
                return cont ? cont : std::noop_coroutine();
            }
            void await_resume() const noexcept {}
        };
        FinalAwaiter final_suspend() noexcept { return {}; }

        void return_void() noexcept {}
        void unhandled_exception() noexcept { exception_ = std::current_exception(); }
    };

    using handle_type = std::coroutine_handle<promise_type>;

    Task() noexcept = default;
    explicit Task(handle_type h) noexcept : handle_(h) {}
    Task(Task&& other) noexcept : handle_(std::exchange(other.handle_, handle_type{})) {}

    Task& operator=(Task&& other) noexcept {
        if (this != &other) {
            if (handle_) handle_.destroy();
            handle_ = std::exchange(other.handle_, handle_type{});
        }
        return *this;
    }
    Task(const Task&) = delete;
    Task& operator=(const Task&) = delete;

    ~Task() {
        if (handle_) handle_.destroy();
    }

    void start() {
        if (handle_ && !handle_.done()) handle_.resume();
    }
    bool done() const noexcept { return !handle_ || handle_.done(); }

    bool await_ready() const noexcept { return false; }
    std::coroutine_handle<> await_suspend(std::coroutine_handle<> caller) noexcept {
        handle_.promise().continuation_ = caller;
        return handle_;
    }
    void await_resume() {
        if (handle_.promise().exception_) {
            std::rethrow_exception(handle_.promise().exception_);
        }
    }

private:
    handle_type handle_{};
};

// ===========================================================================
//  DetachedTask —— 立即启动、结束自毁的"发射后不管"协程
//
//  用于顶层入口（如"每个连接起一个协程"）：
//    - initial_suspend = suspend_never：函数体立刻执行，跑到第一个挂起点；
//    - final_suspend   = suspend_never：协程结束时**自动销毁执行帧**，
//      调用方不需要（也不能）持有句柄 —— 这是"自持有"的关键。
// ===========================================================================
class DetachedTask {
public:
    struct promise_type {
        DetachedTask get_return_object() noexcept { return {}; }
        std::suspend_never initial_suspend() noexcept { return {}; }  // 立即执行
        std::suspend_never final_suspend() noexcept { return {}; }    // 结束自毁
        void return_void() noexcept {}

        void unhandled_exception() noexcept {
            // 不能把异常抛给事件循环（会 std::terminate），这里就地记录
            try {
                std::rethrow_exception(std::current_exception());
            } catch (const std::exception& e) {
                LOG_ERROR("DetachedTask unhandled exception: " << e.what());
            } catch (...) {
                LOG_ERROR("DetachedTask unhandled exception: unknown");
            }
        }
    };
};
