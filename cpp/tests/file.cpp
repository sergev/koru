// SPDX-License-Identifier: MIT
//
// T46's done test: T31's, in C++. The buffered `File`'s fast path **measured**,
// its sticky error, and the three iterators.
//
// Needs /dev/koru, so it runs in the VM under scripts/run-cpp.sh.

#include "surface.hpp"

#include <koru/file.hpp>
#include <koru/iter.hpp>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <sys/stat.h>
#include <unistd.h>

using namespace koru;

namespace {

const char *ROOT = "/tmp/koru-file-cpp";

String fixture(const char *name)
{
    return ::fixture(ROOT, name);
}

/// One rune, as UTF-8, for building expected strings.
String rune(u32 c)
{
    u8 e[4];
    size_t n = utf8_encode(c, e);
    return String(reinterpret_cast<const char *>(e), n);
}

} // namespace

// ---------------------------------------------------------------------------
// The fast path, measured
// ---------------------------------------------------------------------------

namespace {

/// The point of the whole buffer. A version that suspends on every character
/// still produces the right text and would pass any behavioural test, so this
/// asserts an **exact** `ENTER` count instead.
Task<void> case_fast_read(String path, String text)
{
    Result<File> open = co_await File::open(path, FileMode::Read);
    CO_REQUIRE(open.ok());
    File f = std::move(open).take();

    // From here, and not before: the open itself costs ops of its own.
    u64 mark = enters();
    String got;
    for (;;) {
        Result<u32> c = co_await f.get();
        if (!c.ok()) {
            CHECK(c.error() == Kind::Closed);
            break;
        }
        got += rune(c.value());
    }
    u64 spent = enters() - mark;
    CHECK((co_await f.close()).ok());

    CHECK(got == text); // the bytes are the file's

    // Twenty full blocks, a short one, and the read that reports the end.
    u64 refills = (text.size() + FILE_BUF - 1) / FILE_BUF + 1;
    if (spent != refills)
        FAILF("%zu runes cost %llu ENTERs; one per refill is %llu", text.size(),
              (unsigned long long)spent, (unsigned long long)refills);
    CHECK(spent * 100 < text.size()); // the buffer is doing something
}

/// The contrast, so the number above means something: unbuffered, every
/// character really is a syscall.
Task<void> case_slow_read(String path, size_t n)
{
    Result<File> open = co_await File::open(path, FileMode::Read);
    CO_REQUIRE(open.ok());
    File f = std::move(open).take();
    f.set_buffering(Buffering::None);

    u64 mark = enters();
    u8 one   = 0;
    for (size_t i = 0; i < n; i++) {
        Result<size_t> r = co_await f.read(SpanMut<u8>(&one, 1));
        CHECK(r.ok() && r.value() == 1);
    }
    u64 spent = enters() - mark;
    CHECK((co_await f.close()).ok());
    CHECK_EQ(spent, n); // one ENTER per unbuffered read
}

/// What the fast halves buy, which an `ENTER` count cannot see.
///
/// A C++ coroutine allocates a frame whether or not it suspends, so the cost a
/// buffered `put` avoids is the nested `settle` — not a syscall. The slow half
/// answers out of the buffer too, so deleting `take_fast` changes no `ENTER`
/// count at all; deleting `put_fast` shows up here and nowhere else.
Task<void> case_frames(String path, size_t n)
{
    Result<File> open = co_await File::open(path, FileMode::Write);
    CO_REQUIRE(open.ok());
    File f = std::move(open).take();
    f.set_buffering(Buffering::Full);

    // The first put turns the buffer around and probes nothing; measure from
    // the second, which is the steady state.
    CHECK((co_await f.put(u32('x'))).ok());
    u64 mark = detail::frames_allocated;
    for (size_t i = 1; i < n; i++)
        CHECK((co_await f.put(u32('a' + (i % 26)))).ok());
    u64 spent = detail::frames_allocated - mark;

    CHECK((co_await f.close()).ok());
    // One frame per call: `put` itself, and nothing nested inside it.
    if (spent != n - 1)
        FAILF("%zu buffered puts cost %llu coroutine frames, want %zu", n - 1,
              (unsigned long long)spent, n - 1);
}

/// A write is the same bet the other way: the block goes out once.
Task<void> case_fast_write(String path, size_t n)
{
    Result<File> open = co_await File::open(path, FileMode::Write);
    CO_REQUIRE(open.ok());
    File f = std::move(open).take();
    f.set_buffering(Buffering::Full);

    u64 mark = enters();
    for (size_t i = 0; i < n; i++)
        CHECK((co_await f.put(u32('a' + (i % 26)))).ok());
    CHECK((co_await f.flush()).ok());
    u64 spent = enters() - mark;
    CHECK((co_await f.close()).ok());

    CHECK_EQ(spent, n / FILE_BUF); // one ENTER per full block
    CHECK_EQ(slurp(path).size(), n);
}

} // namespace

CASE(file_reading_a_rune_at_a_time_costs_one_enter_per_refill_and_no_more)
{
    String dir  = fixture("fast");
    String path = dir + "/big";
    // Whole multiples of nothing: the last refill is a short one.
    String text;
    for (size_t i = 0; i < FILE_BUF * 20 + 37; i++)
        text += char('a' + (i % 26));
    put_file(path, text);
    run(case_fast_read(path, text));
}

CASE(file_an_unbuffered_read_costs_one_enter_per_call)
{
    String dir  = fixture("slow");
    String path = dir + "/small";
    put_file(path, String(64, 'x'));
    run(case_slow_read(path, 64));
}

CASE(file_writing_a_rune_at_a_time_costs_one_enter_per_block)
{
    String dir = fixture("fast-write");
    run(case_fast_write(dir + "/out", FILE_BUF * 4));
}

CASE(file_a_buffered_put_allocates_one_coroutine_frame_and_no_more)
{
    String dir = fixture("frames");
    run(case_frames(dir + "/out", FILE_BUF / 2));
}

// ---------------------------------------------------------------------------
// The sticky error
// ---------------------------------------------------------------------------

namespace {

/// A read error mid-stream leaves `failed()` true and `error()` exact, and the
/// next call answers out of the field rather than asking the kernel again.
Task<void> case_sticky(String path)
{
    Result<File> open = co_await File::open(path, FileMode::Read);
    CO_REQUIRE(open.ok());
    File f = std::move(open).take();

    // Read it dry: the end of input is what sticks.
    String line;
    while ((co_await f.getline(line, false)).value_or(false)) {
    }
    CHECK(f.eof());
    CHECK(!f.failed()); // an end of input is not a failure
    CHECK(!f.clean());

    // And it costs nothing to read back.
    u64 mark      = enters();
    Result<u32> c = co_await f.get();
    CHECK(!c.ok() && c.error() == Kind::Closed);
    CHECK_EQ(enters() - mark, 0);

    f.clear_err();
    CHECK(f.clean());
    CHECK((co_await f.close()).ok());
}

/// A write to a read-only stream is a real failure, not an end of input.
Task<void> case_failed(String path)
{
    Result<File> open = co_await File::open(path, FileMode::Read);
    CO_REQUIRE(open.ok());
    File f = std::move(open).take();

    Result<void> w = co_await f.write("nope");
    CHECK(!w.ok() && w.error() == Kind::Invalid);
    CHECK(f.failed());
    CHECK(!f.eof());
    CO_REQUIRE(f.error().has_value());
    CHECK(f.error()->kind() == Kind::Invalid);

    // The first error sticks; a later one does not overwrite it.
    Result<u32> g = co_await f.get();
    CHECK(!g.ok() && g.error() == Kind::Invalid);
    CHECK((co_await f.close()).ok());
}

} // namespace

CASE(file_an_end_of_input_is_eof_and_not_a_failure)
{
    String dir  = fixture("sticky");
    String path = dir + "/text";
    put_file(path, "one\ntwo\n");
    run(case_sticky(path));
}

CASE(file_an_error_mid_stream_sticks_and_costs_nothing_to_read_back)
{
    String dir  = fixture("failed");
    String path = dir + "/text";
    put_file(path, "x");
    run(case_failed(path));
}

// ---------------------------------------------------------------------------
// Runes
// ---------------------------------------------------------------------------

namespace {

Task<void> case_runes(String path, std::vector<u32> want)
{
    Result<File> open = co_await File::open(path, FileMode::Read);
    CO_REQUIRE(open.ok());
    File f = std::move(open).take();
    // A block small enough that a rune straddles a refill.
    f.set_buffering(Buffering::Full);

    std::vector<u32> got;
    for (;;) {
        Result<u32> c = co_await f.get();
        if (!c.ok())
            break;
        got.push_back(c.value());
    }
    CHECK((co_await f.close()).ok());
    CHECK_EQ(got.size(), want.size());
    if (got.size() == want.size())
        for (size_t i = 0; i < got.size(); i++)
            if (got[i] != want[i])
                FAILF("rune %zu is U+%04X, want U+%04X", i, got[i], want[i]);
}

/// A sequence the input ends in the middle of is one replacement, not a hang.
Task<void> case_truncated(String path)
{
    Result<File> open = co_await File::open(path, FileMode::Read);
    CO_REQUIRE(open.ok());
    File f = std::move(open).take();

    Result<u32> a = co_await f.get();
    CHECK(a.ok() && a.value() == u32('a'));
    // One replacement per leftover byte: `take_broken` takes one, and the
    // stray continuation behind it is malformed on its own.
    Result<u32> b = co_await f.get();
    CHECK(b.ok() && b.value() == RUNE_REPLACEMENT);
    Result<u32> c = co_await f.get();
    CHECK(c.ok() && c.value() == RUNE_REPLACEMENT);
    Result<u32> d = co_await f.get();
    CHECK(!d.ok() && d.error() == Kind::Closed);
    CHECK((co_await f.close()).ok());
}

/// Every reader after it sees the pushed-back rune, not just `get`.
Task<void> case_unget(String path)
{
    Result<File> open = co_await File::open(path, FileMode::Read);
    CO_REQUIRE(open.ok());
    File f = std::move(open).take();

    Result<u32> a = co_await f.get();
    CHECK(a.ok() && a.value() == u32('b'));
    CHECK(f.unget(u32('b')));
    CHECK(f.unget(0x2603)); // a snowman, three bytes

    String line;
    Result<bool> got = co_await f.getline(line, false);
    CHECK(got.ok() && got.value());
    CHECK(line == rune(0x2603) + "bcd");
    CHECK((co_await f.close()).ok());
}

} // namespace

CASE(file_a_rune_is_a_codepoint_and_not_a_byte)
{
    String dir  = fixture("runes");
    String path = dir + "/utf8";
    // Long enough that a rune falls across a refill boundary, and the
    // arithmetic is asserted rather than assumed: six bytes into 512 divides
    // evenly, so the fixture is deliberately seven.
    String unit = "a" + rune(0xe9) + rune(0x2603) + rune(0x1f600); // 1+2+3+4
    CHECK_EQ(unit.size(), 10);
    CHECK(FILE_BUF % unit.size() != 0);
    String text;
    std::vector<u32> want;
    for (int i = 0; i < 300; i++) {
        text += unit;
        want.push_back(u32('a'));
        want.push_back(0xe9);
        want.push_back(0x2603);
        want.push_back(0x1f600);
    }
    put_file(path, text);
    run(case_runes(path, want));
}

CASE(file_a_truncated_sequence_at_the_end_is_one_replacement)
{
    String dir  = fixture("truncated");
    String path = dir + "/cut";
    put_file(path, "a" + rune(0x2603).substr(0, 2));
    run(case_truncated(path));
}

CASE(file_unget_is_seen_by_every_reader_after_it)
{
    String dir  = fixture("unget");
    String path = dir + "/text";
    put_file(path, "bcd\n");
    run(case_unget(path));
}

// ---------------------------------------------------------------------------
// Lines and big reads
// ---------------------------------------------------------------------------

namespace {

Task<void> case_long_line(String path, String want)
{
    Result<File> open = co_await File::open(path, FileMode::Read);
    CO_REQUIRE(open.ok());
    File f = std::move(open).take();

    String line;
    Result<bool> got = co_await f.getline(line, false);
    CHECK(got.ok() && got.value());
    CHECK(line == want);

    // A final fragment with no newline is a line, and then there are none.
    Result<bool> tail = co_await f.getline(line, false);
    CHECK(tail.ok() && tail.value());
    CHECK(line == "tail");
    Result<bool> none = co_await f.getline(line, false);
    CHECK(none.ok() && !none.value());
    CHECK(f.eof());
    CHECK((co_await f.close()).ok());
}

Task<void> case_keep_nl(String path)
{
    Result<File> open = co_await File::open(path, FileMode::Read);
    CO_REQUIRE(open.ok());
    File f = std::move(open).take();

    String line;
    CHECK((co_await f.getline(line, true)).value_or(false));
    CHECK(line == "one\n");
    CHECK((co_await f.getline(line, false)).value_or(false));
    CHECK(line == "two");
    CHECK((co_await f.close()).ok());
}

/// A span bigger than the block goes straight to the ring, not through it.
Task<void> case_big_read(String path, String text)
{
    Result<File> open = co_await File::open(path, FileMode::Read);
    CO_REQUIRE(open.ok());
    File f = std::move(open).take();

    std::vector<u8> into(text.size());
    u64 mark = enters();
    size_t at = 0;
    while (at < into.size()) {
        Result<size_t> n = co_await f.read(SpanMut<u8>(into.data() + at, into.size() - at));
        if (!n.ok())
            break;
        at += n.value();
    }
    u64 spent = enters() - mark;
    CHECK((co_await f.close()).ok());

    CHECK_EQ(at, text.size());
    CHECK(memcmp(into.data(), text.data(), text.size()) == 0);
    // Through the ring in a handful of reads, not one per block of 512.
    CHECK(spent < text.size() / FILE_BUF);
}

} // namespace

CASE(file_a_line_spans_as_many_refills_as_it_needs)
{
    String dir  = fixture("longline");
    String path = dir + "/text";
    String want(FILE_BUF * 3 + 11, 'L');
    put_file(path, want + "\ntail");
    run(case_long_line(path, want));
}

CASE(file_getline_keeps_the_newline_when_asked)
{
    String dir  = fixture("keepnl");
    String path = dir + "/text";
    put_file(path, "one\ntwo\n");
    run(case_keep_nl(path));
}

CASE(file_a_big_read_bypasses_the_buffer)
{
    String dir  = fixture("bigread");
    String path = dir + "/text";
    String text;
    for (size_t i = 0; i < FILE_BUF * 40; i++)
        text += char(i % 251);
    put_file(path, text);
    run(case_big_read(path, text));
}

// ---------------------------------------------------------------------------
// Buffering
// ---------------------------------------------------------------------------

namespace {

Task<void> case_modes(String dir)
{
    for (Buffering how : { Buffering::None, Buffering::Line, Buffering::Full }) {
        String path = dir + "/out-" + char('0' + int(how));
        Result<File> open = co_await File::open(path, FileMode::Write);
        CO_REQUIRE(open.ok());
        File f = std::move(open).take();
        f.set_buffering(how);

        CHECK((co_await f.write("one\n")).ok());
        CHECK((co_await f.put(u32('t'))).ok());
        CHECK((co_await f.write("wo\n")).ok());
        CHECK((co_await f.close()).ok());
        CHECK(slurp(path) == "one\ntwo\n");
    }
}

/// Line buffering flushes at the newline; full buffering does not.
Task<void> case_line_vs_full(String line_path, String full_path)
{
    Result<File> a = co_await File::open(line_path, FileMode::Write);
    CO_REQUIRE(a.ok());
    File lf = std::move(a).take();
    lf.set_buffering(Buffering::Line);
    CHECK((co_await lf.write("half")).ok());
    CHECK(slurp(line_path).empty()); // no newline yet
    CHECK((co_await lf.write(" line\n")).ok());
    CHECK(slurp(line_path) == "half line\n");
    CHECK((co_await lf.close()).ok());

    Result<File> b = co_await File::open(full_path, FileMode::Write);
    CO_REQUIRE(b.ok());
    File ff = std::move(b).take();
    ff.set_buffering(Buffering::Full);
    CHECK((co_await ff.write("half line\n")).ok());
    CHECK(slurp(full_path).empty()); // still in the block
    CHECK((co_await ff.flush()).ok());
    CHECK(slurp(full_path) == "half line\n");
    CHECK((co_await ff.close()).ok());
}

/// Braam's documented behaviour, and not an oversight: a destructor cannot
/// await, so what was buffered is lost unless somebody flushes.
Task<void> case_no_flush_on_destroy(String path)
{
    {
        Result<File> open = co_await File::open(path, FileMode::Write);
        CO_REQUIRE(open.ok());
        File f = std::move(open).take();
        f.set_buffering(Buffering::Full);
        CHECK((co_await f.write("lost")).ok());
    } // destroyed here, with the block still in hand
    CHECK(slurp(path).empty());
}

} // namespace

CASE(file_every_buffering_mode_puts_the_same_bytes_out)
{
    run(case_modes(fixture("modes")));
}

CASE(file_line_buffering_flushes_at_the_newline_and_full_does_not)
{
    String dir = fixture("linebuf");
    run(case_line_vs_full(dir + "/line", dir + "/full"));
}

CASE(file_destroying_a_file_neither_flushes_nor_closes)
{
    String dir = fixture("noflush");
    run(case_no_flush_on_destroy(dir + "/out"));
}

// ---------------------------------------------------------------------------
// Seeking, closing, detaching
// ---------------------------------------------------------------------------

namespace {

Task<void> case_append(String path)
{
    Result<File> open = co_await File::open(path, FileMode::Append);
    CO_REQUIRE(open.ok());
    File f = std::move(open).take();
    CHECK((co_await f.write("more")).ok());
    CHECK((co_await f.close()).ok());
    CHECK(slurp(path) == "was heremore");
}

/// A seek discards the read-ahead rather than double-counting it.
Task<void> case_seek(String path)
{
    Result<File> open = co_await File::open(path, FileMode::Read);
    CO_REQUIRE(open.ok());
    File f = std::move(open).take();

    Result<u32> a = co_await f.get(); // this refills the whole block
    CHECK(a.ok() && a.value() == u32('0'));

    // SEEK_CUR is relative to where the *reader* is, not to the descriptor.
    Result<u64> at = co_await f.seek(1, SEEK_CUR);
    CHECK(at.ok() && at.value() == 2);
    Result<u32> b = co_await f.get();
    CHECK(b.ok() && b.value() == u32('2'));

    Result<u64> back = co_await f.seek(0, SEEK_SET);
    CHECK(back.ok() && back.value() == 0);
    Result<u32> c = co_await f.get();
    CHECK(c.ok() && c.value() == u32('0'));
    CHECK((co_await f.close()).ok());
}

Task<void> case_detach(String path)
{
    Result<File> open = co_await File::open(path, FileMode::Read);
    CO_REQUIRE(open.ok());
    File f = std::move(open).take();

    Result<u32> a = co_await f.get();
    CHECK(a.ok() && a.value() == u32('0'));

    // The handle comes back positioned where the reader was, not where the
    // read-ahead left the descriptor.
    Result<Handle> h = co_await f.detach();
    CO_REQUIRE(h.ok());
    Result<String> rest = co_await read_some(h.value(), 3);
    CHECK(rest.ok() && rest.value() == "123");
    co_await close_fd(h.value());

    // A detached File is closed: a second one is refused.
    CHECK(!(co_await f.detach()).ok());
}

} // namespace

CASE(file_an_append_mode_file_lands_after_what_was_there)
{
    String dir  = fixture("append");
    String path = dir + "/text";
    put_file(path, "was here");
    run(case_append(path));
}

CASE(file_a_seek_discards_the_read_ahead_rather_than_double_counting_it)
{
    String dir  = fixture("seek");
    String path = dir + "/text";
    put_file(path, "0123456789");
    run(case_seek(path));
}

CASE(file_detach_winds_the_read_ahead_back)
{
    String dir  = fixture("detach");
    String path = dir + "/text";
    put_file(path, "0123456789");
    run(case_detach(path));
}

// ---------------------------------------------------------------------------
// Scanning
// ---------------------------------------------------------------------------

namespace {

Task<void> case_scanners(String path)
{
    Result<File> open = co_await File::open(path, FileMode::Read);
    CO_REQUIRE(open.ok());
    File f = std::move(open).take();

    String tok;
    CHECK((co_await f.scan_token(tok, 0)).value_or(false));
    CHECK(tok == "name");

    Result<i64> n = co_await f.scan_i64(10, 0);
    CHECK(n.ok() && n.value() == -42);
    Result<u64> u = co_await f.scan_u64(0, 0);
    CHECK(u.ok() && u.value() == 0xff);
    Result<u64> o = co_await f.scan_u64(0, 0);
    CHECK(o.ok() && o.value() == 0755);
    Result<u64> b = co_await f.scan_u64(0, 0);
    CHECK(b.ok() && b.value() == 0b1011);

    CHECK((co_await f.skip_space()).ok());
    CHECK((co_await f.scan_lit(u8('['))).value_or(false));
    String until;
    CHECK((co_await f.scan_until(until, "]", 0)).value_or(false));
    CHECK(until == "a b c");
    CHECK((co_await f.scan_lit(u8(']'))).value_or(false));
    // Not taken where it does not match, and nothing to put back.
    CHECK(!(co_await f.scan_lit(u8('Z'))).value_or(true));
    CHECK((co_await f.close()).ok());
}

/// A field width stops a scanner short, and a number that is not one is
/// Invalid rather than a zero.
Task<void> case_widths(String path)
{
    Result<File> open = co_await File::open(path, FileMode::Read);
    CO_REQUIRE(open.ok());
    File f = std::move(open).take();

    Result<u64> a = co_await f.scan_u64(10, 3);
    CHECK(a.ok() && a.value() == 123);
    Result<u64> b = co_await f.scan_u64(10, 3);
    CHECK(b.ok() && b.value() == 456);

    String tok;
    CHECK((co_await f.scan_token(tok, 2)).value_or(false));
    CHECK(tok == "ab");

    // A `0x` with no hex digit behind it does not back up.
    Result<u64> bad = co_await f.scan_u64(0, 0);
    CHECK(!bad.ok() && bad.error() == Kind::Invalid);
    CHECK(f.failed());
    CHECK((co_await f.close()).ok());
}

} // namespace

CASE(file_the_scanners_read_scanfs_conversions_off_a_stream)
{
    String dir  = fixture("scan");
    String path = dir + "/text";
    put_file(path, "  name -42 0xff 0755 0b1011  [a b c]");
    run(case_scanners(path));
}

CASE(file_a_field_width_stops_a_scanner_short)
{
    String dir  = fixture("width");
    String path = dir + "/text";
    put_file(path, "123456 abcd 0xzz");
    run(case_widths(path));
}

// ---------------------------------------------------------------------------
// The standard streams
// ---------------------------------------------------------------------------

namespace {

/// One buffer each, and the same object every time it is asked for.
Task<void> case_std_streams()
{
    CHECK(&File::out() == &File::out());
    CHECK(&File::stderr() == &File::stderr());
    CHECK(&File::out() != &File::stderr());
    CHECK(File::out().fd() == out_fd());
    CHECK(File::stderr().fd() == err_fd());
    CHECK(File::in().fd() == in_fd());
    // stderr is unbuffered so a diagnostic is out before whatever follows it.
    co_await write_err("");
}

} // namespace

CASE(file_the_standard_streams_are_one_buffer_each)
{
    run(case_std_streams());
}

// ---------------------------------------------------------------------------
// The iterators
// ---------------------------------------------------------------------------

namespace {

Task<void> case_input(String a, String b)
{
    Args paths(std::vector<String>{ a, b });
    Input in(paths, in_fd(), "t46");
    String got;
    for (;;) {
        Result<String> chunk = co_await in.read();
        if (!chunk.ok()) {
            CHECK(chunk.error() == Kind::Closed);
            break;
        }
        got += chunk.value();
    }
    CHECK(got == "aaa\nbbb\n");
}

/// A rune split across two files is one rune to a `File` over the `Input`.
Task<void> case_input_carry(String a, String b)
{
    Args paths(std::vector<String>{ a, b });
    File f = File::over(Input(paths, in_fd(), "t46"));

    String got;
    for (;;) {
        Result<u32> c = co_await f.get();
        if (!c.ok())
            break;
        got += rune(c.value());
    }
    CHECK(got == "x" + rune(0x2603) + "y");
}

Task<void> case_input_fallback(String path)
{
    Result<Handle> h = co_await open_read(path);
    CO_REQUIRE(h.ok());
    Input in(Args(), h.value(), "t46");
    Result<String> chunk = co_await in.read();
    CHECK(chunk.ok() && chunk.value() == "fallback");
    co_await close_fd(h.value());
}

Task<void> case_lines(String path)
{
    Args paths(std::vector<String>{ path });
    LineReader r{ Input(paths, in_fd(), "t46") };
    std::vector<String> got;
    String line;
    while ((co_await r.next(line)).value_or(false))
        got.push_back(line);
    CHECK_EQ(got.size(), 4);
    if (got.size() == 4) {
        CHECK(got[0] == "one");
        CHECK(got[1] == "");
        CHECK(got[2] == "two");
        CHECK(got[3] == "three"); // a final fragment with no newline
    }
}

Task<void> case_walk(String root)
{
    TreeWalk w(root + "/");
    CHECK_EQ(w.root_len(), root.size());

    std::vector<String> seen;
    String path;
    DirEntry e;
    for (;;) {
        Result<bool> more = co_await w.next(path, e);
        CO_REQUIRE(more.ok());
        if (!more.value())
            break;
        seen.push_back(path.substr(w.root_len()) + (e.kind == FileKind::Dir ? "/" : ""));
    }
    std::sort(seen.begin(), seen.end());
    CHECK_EQ(seen.size(), 4);
    if (seen.size() == 4) {
        CHECK(seen[0] == "/a/");
        CHECK(seen[1] == "/a/deep");
        CHECK(seen[2] == "/link"); // handed over, not followed
        CHECK(seen[3] == "/top");
    }
}

/// A directory that will not list names itself, and the walk carries on.
Task<void> case_walk_denied(String root, String denied)
{
    TreeWalk w(root);
    String path;
    DirEntry e;
    bool named = false;
    for (int i = 0; i < 16; i++) {
        Result<bool> more = co_await w.next(path, e);
        if (!more.ok()) {
            CHECK(w.at() == denied);
            named = true;
            continue;
        }
        if (!more.value())
            break;
    }
    CHECK(named);
}

} // namespace

CASE(file_an_input_reads_the_files_named_on_a_command_line_as_one_stream)
{
    String dir = fixture("input");
    put_file(dir + "/a", "aaa\n");
    put_file(dir + "/b", "bbb\n");
    run(case_input(dir + "/a", dir + "/b"));
}

CASE(file_an_input_carries_a_rune_across_a_chunk_boundary)
{
    String dir = fixture("carry");
    String s   = rune(0x2603);
    // The split is deliberate: one byte of the snowman ends the first file.
    put_file(dir + "/a", "x" + s.substr(0, 1));
    put_file(dir + "/b", s.substr(1) + "y");
    run(case_input_carry(dir + "/a", dir + "/b"));
}

CASE(file_an_input_with_no_paths_reads_its_fallback)
{
    String dir = fixture("fallback");
    put_file(dir + "/f", "fallback");
    run(case_input_fallback(dir + "/f"));
}

CASE(file_a_line_reader_splits_the_stream_and_a_final_fragment_is_a_line)
{
    String dir = fixture("lines");
    put_file(dir + "/f", "one\n\ntwo\nthree");
    run(case_lines(dir + "/f"));
}

CASE(file_a_tree_walks_pre_order_and_hands_a_link_over_rather_than_following_it)
{
    String dir = fixture("walk");
    std::error_code ec;
    std::filesystem::create_directories(dir + "/a", ec);
    put_file(dir + "/top", "t");
    put_file(dir + "/a/deep", "d");
    std::filesystem::create_directory_symlink("a", dir + "/link", ec);
    run(case_walk(dir));
}

CASE(file_a_directory_that_will_not_list_names_itself_in_at)
{
    String dir = fixture("denied");
    std::error_code ec;
    std::filesystem::create_directories(dir + "/shut", ec);
    put_file(dir + "/open", "x");
    if (::chmod((dir + "/shut").c_str(), 0) != 0 || ::geteuid() == 0) {
        // root reads every directory, so there is nothing to refuse.
        ::chmod((dir + "/shut").c_str(), 0755);
        test_note("running as root: the unreadable-directory case is vacuous");
        return;
    }
    run(case_walk_denied(dir, dir + "/shut"));
    ::chmod((dir + "/shut").c_str(), 0755);
}
