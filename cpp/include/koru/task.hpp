// SPDX-License-Identifier: MIT
//
// `task<T>`: lazy, move-only, and transferring symmetrically.
//
// Three properties, each load-bearing:
//
//   - **Lazy.** `initial_suspend` is `suspend_always`, so nothing runs until
//     the task is awaited or handed to `sync_wait`. Ownership is explicit and
//     the classic detached-coroutine leak — `run()` returning with frames
//     still suspended — is foreclosed.
//   - **Symmetric transfer.** `final_suspend` returns an awaiter whose
//     `await_suspend` *returns* the continuation handle rather than calling
//     `.resume()` on it, and so does `task::await_suspend`. Without either, a
//     chain of N awaits costs N C++ stack frames. A ten-deep test passes
//     regardless, which is why the done test is a hundred thousand deep and
//     measures the stack rather than trusting it.
//
//     The standard says the continuation is resumed *as if by a tail call*,
//     and clang does it at every optimization level. **GCC does it only at
//     -O2**: at -O0 the done test dies of a stack overflow and at -O1 it
//     measures 64 bytes a level. Build this binding with optimization on.
//   - **Exception-free.** `unhandled_exception` calls `std::terminate`. An
//     exception cannot cross the ABI boundary and a detached task has nowhere
//     to send one; errors travel as `result<T>` values instead.
//
// A frame that cannot be allocated yields a *null* task rather than undefined
// behaviour, which is what makes Braam's pervasive `if (task<T> t = ...)`
// idiom compile and mean something.

#ifndef KORU_TASK_HPP
#define KORU_TASK_HPP

#include <koru/result.hpp>

#include <coroutine>
#include <cstddef>
#include <exception>
#include <new>
#include <optional>
#include <utility>

namespace koru {

namespace detail {

/// Makes the next `n` coroutine-frame allocations fail. Zero everywhere but
/// the one test that has to reach the allocation-failure path, which no
/// ordinary run can.
inline unsigned fail_frame_allocations = 0;

inline void *frame_alloc(size_t n) noexcept
{
    if (fail_frame_allocations) {
        fail_frame_allocations--;
        return nullptr;
    }
    return ::operator new(n, std::nothrow);
}

/// What every task promise has in common: the continuation, the transfer, and
/// the two rules above.
template <class Promise>
struct promise_base {
    std::coroutine_handle<> continuation;

    /// The transfer out. Returning the handle is the whole of symmetric
    /// transfer: the compiler tail-calls it, so the stack does not grow.
    struct final_awaiter {
        bool await_ready() const noexcept { return false; }

        std::coroutine_handle<> await_suspend(std::coroutine_handle<Promise> h) const noexcept
        {
            std::coroutine_handle<> c = h.promise().continuation;
            // `noop_coroutine` is what a task nobody is waiting for returns
            // to: the resume that started it.
            return c ? c : std::noop_coroutine();
        }

        void await_resume() const noexcept {}
    };

    std::suspend_always initial_suspend() const noexcept { return {}; }
    final_awaiter final_suspend() const noexcept { return {}; }
    void unhandled_exception() const noexcept { std::terminate(); }

    static void *operator new(size_t n) noexcept { return detail::frame_alloc(n); }
    static void operator delete(void *p) noexcept { ::operator delete(p); }
};

} // namespace detail

template <class T = void>
class task;

template <class T>
class task {
public:
    struct promise_type : detail::promise_base<promise_type> {
        std::optional<T> value;

        task get_return_object()
        {
            return task(std::coroutine_handle<promise_type>::from_promise(*this));
        }

        /// A frame that would not allocate. The caller sees a null task, which
        /// is a value rather than undefined behaviour.
        static task get_return_object_on_allocation_failure() { return task(); }

        template <class U>
        void return_value(U &&v)
        {
            value.emplace(std::forward<U>(v));
        }
    };

    task() = default;
    explicit task(std::coroutine_handle<promise_type> h) : handle_(h) {}

    task(const task &)            = delete;
    task &operator=(const task &) = delete;
    task(task &&o) noexcept : handle_(o.handle_) { o.handle_ = {}; }
    task &operator=(task &&o) noexcept
    {
        if (this != &o) {
            destroy();
            handle_   = o.handle_;
            o.handle_ = {};
        }
        return *this;
    }

    ~task() { destroy(); }

    /// Braam's `if (task<T> t = ...)`: false is a frame that could not be
    /// allocated, or a task already moved from.
    explicit operator bool() const { return bool(handle_); }

    bool done() const { return !handle_ || handle_.done(); }

    /// The handle, for an executor that starts and owns it.
    std::coroutine_handle<promise_type> handle() const { return handle_; }

    /// Gives the frame up to the caller, which then owns it.
    std::coroutine_handle<promise_type> release()
    {
        std::coroutine_handle<promise_type> h = handle_;
        handle_                               = {};
        return h;
    }

    // ------------------------------------------------------------ awaitable

    bool await_ready() const { return !handle_ || handle_.done(); }

    std::coroutine_handle<> await_suspend(std::coroutine_handle<> caller)
    {
        handle_.promise().continuation = caller;
        // The callee, not `void`: this is the transfer *in*, and calling
        // `.resume()` here instead is the other half of the stack leak.
        return handle_;
    }

    T await_resume()
    {
        if (!handle_ || !handle_.promise().value)
            fail("koru::task: awaited a task that returned nothing");
        return std::move(*handle_.promise().value);
    }

private:
    void destroy()
    {
        if (handle_) {
            handle_.destroy();
            handle_ = {};
        }
    }

    std::coroutine_handle<promise_type> handle_;
};

template <>
class task<void> {
public:
    struct promise_type : detail::promise_base<promise_type> {
        task get_return_object()
        {
            return task(std::coroutine_handle<promise_type>::from_promise(*this));
        }
        static task get_return_object_on_allocation_failure() { return task(); }
        void return_void() {}
    };

    task() = default;
    explicit task(std::coroutine_handle<promise_type> h) : handle_(h) {}

    task(const task &)            = delete;
    task &operator=(const task &) = delete;
    task(task &&o) noexcept : handle_(o.handle_) { o.handle_ = {}; }
    task &operator=(task &&o) noexcept
    {
        if (this != &o) {
            destroy();
            handle_   = o.handle_;
            o.handle_ = {};
        }
        return *this;
    }

    ~task() { destroy(); }

    explicit operator bool() const { return bool(handle_); }
    bool done() const { return !handle_ || handle_.done(); }
    std::coroutine_handle<promise_type> handle() const { return handle_; }

    std::coroutine_handle<promise_type> release()
    {
        std::coroutine_handle<promise_type> h = handle_;
        handle_                               = {};
        return h;
    }

    bool await_ready() const { return !handle_ || handle_.done(); }

    std::coroutine_handle<> await_suspend(std::coroutine_handle<> caller)
    {
        handle_.promise().continuation = caller;
        return handle_;
    }

    void await_resume() const {}

private:
    void destroy()
    {
        if (handle_) {
            handle_.destroy();
            handle_ = {};
        }
    }

    std::coroutine_handle<promise_type> handle_;
};

/// Run a task to completion here and now.
///
/// It drives nothing but the task: a task that suspends on the reactor needs
/// the executor, and saying so is better than blocking for ever on a ring
/// nobody is pumping.
template <class T>
T sync_wait(task<T> t)
{
    if (!t)
        fail("koru::sync_wait: a task whose frame could not be allocated");
    t.handle().resume();
    if (!t.done())
        fail("koru::sync_wait: the task suspended on something only the executor can complete");
    if constexpr (!std::is_void_v<T>)
        return t.await_resume();
}

} // namespace koru

#endif // KORU_TASK_HPP
