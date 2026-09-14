// SPDX-License-Identifier: MIT
//
// T44's host half: the vocabulary, the `TRY` family and `Args`. None of it
// touches /dev/koru, so ctest runs it on the host.
//
// The errno table itself is T39's, in unit.cpp; what is new here is that every
// name a program can be handed is reachable from that table, and that `TRY`
// converts each of the error types a koru program meets.

#include "harness.hpp"

#include <koru/args.hpp>
#include <koru/task.hpp>
#include <koru/vocab.hpp>

#include <cerrno>
#include <set>
#include <string>

using namespace koru;

namespace {

// A comma inside a template argument list is a comma to the preprocessor, so
// the two-parameter results get a name of their own before they reach a macro.
using ErrnoVoid = result<void, Errno>;
using EnterVoid = result<void, EnterError>;

/// `TRY` from each error type. A missing `as_error` overload fails this at
/// compile time, which is why the bodies assert so little.
result<int> from_errno_value(Errno e)
{
    TRY_VOID(ErrnoVoid(e));
    return 0;
}

result<int> from_enter(EnterError e)
{
    TRY_VOID(EnterVoid(e));
    return 0;
}

result<int> from_error(Error e)
{
    TRY_VOID(result<void>(e));
    return 0;
}

/// And the value-yielding arm, which must move rather than copy.
result<String> try_moves_its_value()
{
    String s = TRY(result<String>(String("a string long enough not to fit in a small buffer")));
    return s;
}

task<result<int>> co_try_from_errno(Errno e)
{
    CO_TRY_VOID(ErrnoVoid(e));
    co_return 0;
}

task<result<String>> co_try_moves_its_value()
{
    String s = CO_TRY(result<String>(String("another string past the small-buffer size")));
    co_return std::move(s);
}

// Braam's signatures, in Braam's aliases. Never called in anger: a drift in
// one of the four is a compile error.
Option<char> first(Str s)
{
    return s.empty() ? Option<char>() : Option<char>(s.front());
}

u32 total(Span<u8> xs)
{
    u32 n = 0;
    for (u8 b : xs)
        n += b;
    return n;
}

void zero(SpanMut<u8> xs)
{
    for (u8 &b : xs)
        b = 0;
}

} // namespace

CASE(vocab_every_errno_converts_through_try_to_the_same_error)
{
    for (size_t i = 0; i < koru_errnos_len(); i++) {
        const ErrnoDef &d = KORU_ERRNOS[i];
        Error want        = Error::from_errno(d.err);

        result<int> a = from_errno_value(d.err);
        result<int> b = from_enter(EnterError{ d.err, Progress{} });
        result<int> c = from_error(want);
        if (a.ok() || b.ok() || c.ok()) {
            FAILF("%s: TRY_VOID let an error through", d.name);
            continue;
        }
        if (!(a.error() == want) || !(b.error() == want) || !(c.error() == want))
            FAILF("%s: TRY_VOID converted to a different error", d.name);

        // And the same inside a coroutine, which is a different statement.
        result<int> co = sync_wait(co_try_from_errno(d.err));
        if (co.ok() || !(co.error() == want))
            FAILF("%s: CO_TRY_VOID disagrees with TRY_VOID", d.name);
    }
}

CASE(vocab_try_yields_the_value_and_moves_it)
{
    result<String> r = try_moves_its_value();
    CHECK(r.ok());
    CHECK(r.value().size() > 32);

    result<String> c = sync_wait(co_try_moves_its_value());
    CHECK(c.ok());
    CHECK(c.value().size() > 32);
}

/// A name no errno reaches is a name no program can be handed. Since T21 there
/// are none: `Closed` arrives as EPIPE from a write, and end of file
/// synthesises it with no errno at all.
CASE(vocab_every_name_is_reachable_from_the_errno_table)
{
    std::set<int> mapped;
    for (size_t i = 0; i < koru_errnos_len(); i++)
        mapped.insert(int(KORU_ERRNOS[i].kind));

    for (size_t i = 0; i < 15; i++)
        if (!mapped.count(int(KINDS[i])))
            FAILF("a name no errno can produce: %s", kind_name(KINDS[i]));

    CHECK(Error::from_errno(Errno(EPIPE)).kind() == Kind::Closed);
    CHECK(Error::closed().kind() == Kind::Closed);
    CHECK(Error::closed().raw() == Errno(0)); // end of file has no errno
}

CASE(vocab_the_aliases_spell_braams_signatures)
{
    u8 buf[3] = { 1, 2, 3 };
    CHECK(first("koru").value() == 'k');
    CHECK(!first("").has_value());
    CHECK_EQ(total(Span<u8>(buf, 3)), 6);
    zero(SpanMut<u8>(buf, 3));
    CHECK_EQ(total(Span<u8>(buf, 3)), 0);
}

// ---------------------------------------------------------------------------
// Args
// ---------------------------------------------------------------------------

namespace {

Args three()
{
    return Args(std::vector<String>{ "hello", "koru", "world" });
}

} // namespace

CASE(args_name_is_the_first_and_tail_drops_it)
{
    Args a = three();
    CHECK_EQ(a.size(), 3);
    CHECK(a.name() == "hello");
    CHECK(a[1] == "koru");

    Args t = a.tail();
    CHECK_EQ(t.size(), 2);
    CHECK(t.name() == "koru");
    CHECK(t[0] == "koru");
    CHECK_EQ(a.size(), 3); // tail shares the vector rather than consuming it
}

CASE(args_an_empty_vector_has_an_empty_name_and_a_tail_that_stays_empty)
{
    Args a;
    CHECK(a.empty());
    CHECK(a.name() == "");
    CHECK(a.tail().empty());
    CHECK(a.tail().tail().empty());
}

CASE(args_tail_past_the_end_is_empty_rather_than_a_fault)
{
    Args a = three().tail().tail().tail().tail();
    CHECK(a.empty());
    CHECK(a.name() == "");
}

CASE(args_skip_is_tail_repeated_and_saturates)
{
    Args a = three();
    CHECK_EQ(a.skip(0).size(), 3);
    CHECK(a.skip(2).name() == "world");
    CHECK(a.skip(3).empty());
    CHECK(a.skip(SIZE_MAX).empty());
}
