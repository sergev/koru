// SPDX-License-Identifier: MIT
//
// Braam's vocabulary in C++: the integer names, the four aliases, and the
// `TRY` family. The C++ half of rust/runtime/src/vocab.rs, and it shares no
// code with it.
//
// `Error`, `Errno` and `Kind` are libkoru's own, from error.hpp — not a second
// type. What this header adds is the spelling a Braam source expects.
//
// The macros are **statement expressions**, which are a GNU extension: this
// binding is built with `-std=gnu++20`. C++ has no other way to spell "unwrap
// or return" as an expression, and `TRY` must be one — `i32 n = TRY(f());` is
// how every Braam program is written.

#ifndef KORU_VOCAB_HPP
#define KORU_VOCAB_HPP

#include <koru/error.hpp>
#include <koru/result.hpp>
#include <koru/ring.hpp>

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>

namespace koru {

// Braam's integer names. It has no libc, so these are its `int` and its
// `unsigned`; a Braam source says `u32` and means exactly this.
using i8  = int8_t;
using i16 = int16_t;
using i32 = int32_t;
using i64 = int64_t;
using u8  = uint8_t;
using u16 = uint16_t;
using u32 = uint32_t;
using u64 = uint64_t;
using f64 = double;

/// Braam's `Str`: a non-owning view of UTF-8 bytes.
using Str = std::string_view;

/// Braam's `String`, which is an owning buffer of unvalidated bytes. C++'s is
/// the same thing, which is why `read_chunk` can hand one back here where the
/// Rust binding has to hand back `Vec<u8>`.
using String = std::string;

/// Braam's `Span<const T>` and `Span<T>`.
template <class T>
using Span = std::span<const T>;

template <class T>
using SpanMut = std::span<T>;

/// Braam's `Option<T>`.
template <class T>
using Option = std::optional<T>;

/// An open file, as the kernel encodes it: index low, generation high. A
/// plain integer rather than a newtype, because Braam's `io.h` spells it
/// `u32 fd` and a program passes it around by that name.
using Handle = uint32_t;

/// What `TRY` converts an error into. One overload per error type a koru
/// program meets, which is the C++ spelling of the `From` impls koru-sys
/// carries for the Rust binding.
inline Error as_error(Error e)
{
    return e;
}

inline Error as_error(Errno e)
{
    return Error::from_errno(e);
}

inline Error as_error(EnterError e)
{
    return Error::from_errno(e.err);
}

} // namespace koru

// ---------------------------------------------------------------------------
// TRY
// ---------------------------------------------------------------------------
//
// Braam's four macros. `TRY` yields the value, `TRY_VOID` yields nothing, and
// the `CO_` pair are the same inside a coroutine — `return` and `co_return`
// are different statements and no macro can be both.
//
// Each **consumes** its argument: the value is moved out. That is what makes
// `String s = TRY(read_file(p));` copy nothing, and it is why passing an
// lvalue leaves that lvalue moved-from.

#define KORU_TRY_IMPL(expr, ret)                       \
    ({                                                 \
        auto &&_koru_r = (expr);                       \
        if (!_koru_r.ok())                             \
            ret ::koru::as_error(_koru_r.error());     \
        std::move(_koru_r).take();                     \
    })

#define KORU_TRY_VOID_IMPL(expr, ret)                  \
    do {                                               \
        auto &&_koru_r = (expr);                       \
        if (!_koru_r.ok())                             \
            ret ::koru::as_error(_koru_r.error());     \
    } while (0)

#define TRY(expr)         KORU_TRY_IMPL(expr, return)
#define TRY_VOID(expr)    KORU_TRY_VOID_IMPL(expr, return)
#define CO_TRY(expr)      KORU_TRY_IMPL(expr, co_return)
#define CO_TRY_VOID(expr) KORU_TRY_VOID_IMPL(expr, co_return)

// ---------------------------------------------------------------------------
// The portable pair
// ---------------------------------------------------------------------------
//
// **`CO_TRY(co_await f())` compiles on clang and not on GCC.** A statement
// expression holding both a `co_await` and a `co_return` is an internal
// compiler error in GCC at every optimization level; doc/Notes.md has the
// fourteen-line reproducer. There is no way to spell an early return from
// inside an *expression* without a braced group, so that one macro is what it
// is, and a Braam source that writes it needs clang.
//
// The `_VOID` pair is unaffected: both are `do { ... } while (0)`, which is a
// statement and not a braced group, so `CO_TRY_VOID(co_await f())` is fine
// everywhere. Only a value coming *out* of the await is the problem.
//
// These two are koru's own, not Braam's, and they are statements for the same
// reason. libkoru's own sources use them wherever a value comes out of an
// await, which is what keeps the library and its whole suite buildable by
// either compiler — and T41's measurement of GCC's symmetric transfer
// re-runnable.
//
// `CO_LET(n, e)` declares `n` from a successful result; `CO_OK(e)` discards
// the value. Both leave the enclosing coroutine on an error.

#define CO_LET(name, expr)                                   \
    auto name##_koru_res = (expr);                           \
    if (!name##_koru_res.ok())                               \
        co_return ::koru::as_error(name##_koru_res.error()); \
    auto name = std::move(name##_koru_res).take()

#define CO_OK(expr)                                        \
    do {                                                   \
        auto _koru_res = (expr);                           \
        if (!_koru_res.ok())                               \
            co_return ::koru::as_error(_koru_res.error()); \
    } while (0)

#endif // KORU_VOCAB_HPP
