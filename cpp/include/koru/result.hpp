// SPDX-License-Identifier: MIT
//
// `result<T, E>`: a value or an error, never both. C++20 has no std::expected,
// which is C++23, and this binding cannot wait for it.
//
// Errors travel as values. An exception cannot cross the ABI boundary and a
// detached coroutine has nowhere to send one, so nothing here throws and
// reading the wrong side aborts rather than returning rubbish.

#ifndef KORU_RESULT_HPP
#define KORU_RESULT_HPP

#include <cstdio>
#include <cstdlib>
#include <new>
#include <type_traits>
#include <utility>

namespace koru {

[[noreturn]] inline void fail(const char *what)
{
    fputs(what, stderr);
    fputc('\n', stderr);
    abort();
}

/// Declared, not defined, so the default can name it without a cycle: the
/// vocabulary type is error.hpp's, and it includes this.
class Error;

template <class T, class E = Error>
class result {
public:
    using value_type = T;
    using error_type = E;

    result(T v) : ok_(true) { new (&val_) T(std::move(v)); }
    result(E e) : ok_(false) { new (&err_) E(std::move(e)); }

    result(const result &o) : ok_(o.ok_)
    {
        if (ok_)
            new (&val_) T(o.val_);
        else
            new (&err_) E(o.err_);
    }

    result(result &&o) noexcept : ok_(o.ok_)
    {
        if (ok_)
            new (&val_) T(std::move(o.val_));
        else
            new (&err_) E(std::move(o.err_));
    }

    result &operator=(result o) noexcept
    {
        destroy();
        ok_ = o.ok_;
        if (ok_)
            new (&val_) T(std::move(o.val_));
        else
            new (&err_) E(std::move(o.err_));
        return *this;
    }

    ~result() { destroy(); }

    bool ok() const { return ok_; }
    explicit operator bool() const { return ok_; }

    T &value() &
    {
        if (!ok_)
            fail("koru::result: value() on an error");
        return val_;
    }

    const T &value() const &
    {
        if (!ok_)
            fail("koru::result: value() on an error");
        return val_;
    }

    // The one way to get the value out of a move-only T.
    T take() &&
    {
        if (!ok_)
            fail("koru::result: take() on an error");
        return std::move(val_);
    }

    T &operator*() { return value(); }
    const T &operator*() const { return value(); }
    T *operator->() { return &value(); }
    const T *operator->() const { return &value(); }

    const E &error() const
    {
        if (ok_)
            fail("koru::result: error() on a value");
        return err_;
    }

    T value_or(T other) const { return ok_ ? val_ : std::move(other); }

private:
    void destroy()
    {
        if (ok_)
            val_.~T();
        else
            err_.~E();
    }

    bool ok_;
    union {
        T val_;
        E err_;
    };
};

// `result<void>`: succeeded, or this is why not.
template <class E>
class result<void, E> {
public:
    using value_type = void;
    using error_type = E;

    result() : ok_(true) {}
    result(E e) : ok_(false), err_(std::move(e)) {}

    bool ok() const { return ok_; }
    explicit operator bool() const { return ok_; }

    const E &error() const
    {
        if (ok_)
            fail("koru::result: error() on a value");
        return err_;
    }

private:
    bool ok_;
    E err_ = {};
};

} // namespace koru

#endif // KORU_RESULT_HPP
