// SPDX-License-Identifier: MIT
//
// T47's host half: the option parser and the calendar. Both are pure, so ctest
// runs them with no VM and no device.
//
// The vectors are Braam's own `test_opt.cpp` and `test_time.cpp`, string for
// string, as the Rust binding's are. The two usage helpers are not here: which
// stream a block goes to and what status follows it is a program's question,
// and `cpp/examples/date.cpp` is what asks it.
//
// It also holds the pure half of T45 and T46 — the flag translation, the
// diagnostic's wording, the dirent walk and the UTF-8 rules — which need no
// ring either.

#include "harness.hpp"

#include <koru/filebuf.hpp>
#include <koru/opt.hpp>
#include <koru/ops.hpp>
#include <koru/time.hpp>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <koru_abi.h>

using namespace koru;

// ---------------------------------------------------------------------------
// The option parser
// ---------------------------------------------------------------------------

namespace {

// ls's letters, and a valued one for head's -n. Braam's own two specs.
const Opts FLAGS{ "1CRSdhlr", "" };
const Opts VALUED{ "v", "n" };

Args argv_of(std::initializer_list<const char *> v)
{
    std::vector<String> out;
    for (const char *s : v)
        out.emplace_back(s);
    return Args(std::move(out));
}

/// Braam's own `scan`, string for string: a bare letter, `letter=value`,
/// `!letter` on an error, then a slash and the operands. One comparison checks
/// the letters, their order, the values and what is left.
String scan(std::initializer_list<const char *> v, Opts spec, Kind *err = nullptr)
{
    Args args = argv_of(v);
    OptParse p(args, spec);
    String out;

    for (;;) {
        Str sep = out.empty() ? "" : ",";
        Opt o;
        result<bool, OptError> got = p.next(o);
        if (!got.ok()) {
            if (err)
                *err = got.error().error.kind();
            out += sep;
            out += '!';
            u8 e[4];
            out.append(reinterpret_cast<const char *>(e), utf8_encode(got.error().name, e));
            break;
        }
        if (!got.value())
            break;
        out += sep;
        u8 e[4];
        out.append(reinterpret_cast<const char *>(e), utf8_encode(o.name, e));
        if (!o.value.empty()) {
            out += '=';
            out += String(o.value);
        }
    }

    out += '/';
    bool first = true;
    for (const String &a : p.rest()) {
        if (!first)
            out += ' ';
        first = false;
        out += a;
    }
    return out;
}

String flags(std::initializer_list<const char *> v)
{
    return scan(v, FLAGS);
}

String valued(std::initializer_list<const char *> v)
{
    return scan(v, VALUED);
}

void want(const String &got, const char *expect)
{
    if (got != expect)
        FAILF("got [%s] want [%s]", got.c_str(), expect);
}

} // namespace

CASE(opt_nothing_at_all_and_operands_with_no_flags)
{
    want(flags({ "ls" }), "/");
    want(flags({ "ls", "a", "b" }), "/a b");
}

CASE(opt_separate_bundled_and_both_read_left_to_right)
{
    want(flags({ "ls", "-l", "-R", "x" }), "l,R/x");
    want(flags({ "ls", "-lR", "x" }), "l,R/x");
    want(flags({ "ls", "-lR", "-S", "x" }), "l,R,S/x");
}

CASE(opt_the_first_operand_ends_the_options)
{
    want(flags({ "ls", "-l", "x", "-R" }), "l/x -R");
}

CASE(opt_a_bare_separator_is_consumed_and_a_lone_dash_is_an_operand)
{
    want(flags({ "ls", "-l", "--", "-R" }), "l/-R");
    want(flags({ "ls", "-", "-l" }), "/- -l");
    want(flags({ "ls", "--" }), "/");
}

CASE(opt_a_valued_letter_takes_the_rest_of_its_word_or_the_next_one)
{
    want(valued({ "head", "-n5", "f" }), "n=5/f");
    want(valued({ "head", "-n", "5", "f" }), "n=5/f");
    // It ends the bundle: what follows the letter is the value.
    want(valued({ "head", "-vn12", "f" }), "v,n=12/f");
    want(valued({ "head", "-vn", "12", "f" }), "v,n=12/f");
    // The operand after a detached value is an operand, not a bundle: Braam's
    // vectors all name a one-letter file and cannot say it.
    want(valued({ "head", "-n", "5", "file" }), "n=5/file");
    want(valued({ "head", "-vn", "12", "file", "x" }), "v,n=12/file x");
}

CASE(opt_a_missing_argument_and_an_unknown_letter_are_two_different_mistakes)
{
    Kind k = Kind::Io;
    want(scan({ "head", "-n" }, VALUED, &k), "!n/");
    CHECK(k == Kind::NotFound);
    want(scan({ "ls", "-z", "x" }, FLAGS, &k), "!z/x");
    CHECK(k == Kind::Invalid);
    // From inside a bundle, and what was read before it still came out.
    want(scan({ "ls", "-lz", "x" }, FLAGS, &k), "l,!z/x");
    CHECK(k == Kind::Invalid);
}

/// Braam's "advanced first": a program that reports a bad letter and carries
/// on reaches the operands rather than looping on the letter.
CASE(opt_a_caller_that_carries_on_past_an_error_still_terminates)
{
    Args args = argv_of({ "ls", "-lzq", "-z", "x" });
    OptParse p(args, FLAGS);
    String bad, good;

    for (int i = 0; i < 16; i++) {
        Opt o;
        result<bool, OptError> got = p.next(o);
        if (!got.ok()) {
            bad += char(got.error().name);
            continue;
        }
        if (!got.value())
            break;
        good += char(o.name);
    }
    want(good, "l");
    want(bad, "zqz"); // a letter was repeated or skipped
    CHECK_EQ(p.rest().size(), 1);
    CHECK(p.rest()[0] == "x");
}

/// A letter outside ASCII is one the program does not take, not a read across
/// a codepoint boundary.
CASE(opt_a_multibyte_letter_is_refused_whole)
{
    Kind k = Kind::Io;
    want(scan({ "ls", "-\xc3\xa9", "x" }, VALUED, &k), "!\xc3\xa9/x");
    CHECK(k == Kind::Invalid);

    Opts spec{ "", "\xc3\xa9" };
    want(scan({ "ls", "-\xc3\xa9"
                      "5",
                "x" },
              spec),
         "\xc3\xa9=5/x");
}

CASE(opt_help_is_asked_only_as_the_whole_line)
{
    CHECK(help_asked(argv_of({ "rm", "-h" })));
    CHECK(help_asked(argv_of({ "rm", "--help" })));
    CHECK(!help_asked(argv_of({ "rm" })));
    // A file named `-h` is still an operand, and a value is never argv[1].
    CHECK(!help_asked(argv_of({ "rm", "-h", "x" })));
    CHECK(!help_asked(argv_of({ "basename", "-s", "-h", "x" })));
    // Neither is any other spelling of it.
    CHECK(!help_asked(argv_of({ "rm", "-help" })));
    CHECK(!help_asked(argv_of({ "rm", "--h" })));
}

// ---------------------------------------------------------------------------
// The calendar
// ---------------------------------------------------------------------------

namespace {

/// Braam's `check`: the fields, the day name, and the round trip.
void check_at(i64 secs, i32 y, u32 mo, u32 d, u32 h, u32 mi, u32 s, const char *day)
{
    Civil c = civil(secs);
    if (c.year != y || c.month != mo || c.day != d || c.hour != h || c.min != mi || c.sec != s)
        FAILF("%lld: %d-%02u-%02u %02u:%02u:%02u, want %d-%02u-%02u %02u:%02u:%02u",
              (long long)secs, c.year, c.month, c.day, c.hour, c.min, c.sec, y, mo, d, h, mi, s);
    if (TIME_DAYS[c.weekday] != Str(day))
        FAILF("%lld: weekday %u", (long long)secs, c.weekday);
    if (civil_secs(c) != secs)
        FAILF("%lld did not round-trip", (long long)secs);
}

i64 at(i32 y, u32 mo, u32 d, u32 h, u32 mi, u32 s)
{
    Civil c;
    c.year  = y;
    c.month = mo;
    c.day   = d;
    c.hour  = h;
    c.min   = mi;
    c.sec   = s;
    return civil_secs(c);
}

} // namespace

CASE(time_the_epoch_and_the_days_either_side_of_it)
{
    check_at(0, 1970, 1, 1, 0, 0, 0, "Thu");
    check_at(86399, 1970, 1, 1, 23, 59, 59, "Thu");
    check_at(86400, 1970, 1, 2, 0, 0, 0, "Fri");
}

CASE(time_a_leap_day_and_the_day_after_it)
{
    check_at(1709208000, 2024, 2, 29, 12, 0, 0, "Thu");
    check_at(1709294400, 2024, 3, 1, 12, 0, 0, "Fri");
}

/// 1900 is not a leap year and 2000 is — the two the era arithmetic exists to
/// get right without a table.
CASE(time_the_century_rule_both_ways)
{
    check_at(-2203891200LL, 1900, 3, 1, 0, 0, 0, "Thu");
    check_at(951782400, 2000, 2, 29, 0, 0, 0, "Tue");
}

/// Before the epoch, where the seconds-in-day remainder goes negative.
CASE(time_before_the_epoch)
{
    check_at(-1, 1969, 12, 31, 23, 59, 59, "Wed");
    check_at(-86400, 1969, 12, 31, 0, 0, 0, "Wed");
}

CASE(time_the_name_tables_are_braams)
{
    CHECK(TIME_MONTHS[0] == "Jan");
    CHECK(TIME_MONTHS[11] == "Dec");
    CHECK(TIME_DAYS[0] == "Thu");
    CHECK(TIME_DAYS[6] == "Wed");
}

/// Out-of-range fields normalise, which is what `mktime` callers rely on.
CASE(time_fields_outside_their_ranges_normalise)
{
    CHECK_EQ(at(1970, 13, 1, 0, 0, 0), at(1971, 1, 1, 0, 0, 0));
    CHECK_EQ(at(1970, 0, 1, 0, 0, 0), at(1969, 12, 1, 0, 0, 0));
    CHECK_EQ(at(1970, 1, 32, 0, 0, 0), at(1970, 2, 1, 0, 0, 0));
    CHECK_EQ(at(1970, 1, 1, 25, 0, 0), 25 * 3600);
    CHECK_EQ(at(2024, 2, 30, 0, 0, 0), at(2024, 3, 1, 0, 0, 0));
    // Day 0 is the last of the month before; Braam's `u64` would wrap.
    CHECK_EQ(at(2024, 3, 0, 0, 0, 0), at(2024, 2, 29, 0, 0, 0));
}

/// Every six hours over a century and a half, both sides of the epoch. The
/// weekday advances by one a day across all of it, which no single vector can
/// say.
CASE(time_every_date_over_a_century_and_a_half_round_trips)
{
    const i64 step = 6 * 3600;
    i64 from       = at(1900, 1, 1, 0, 0, 0);
    i64 to         = at(2050, 1, 1, 0, 0, 0);

    i64 n         = 0;
    bool have_day = false;
    u32 prev_day  = 0;
    for (i64 secs = from; secs < to; secs += step) {
        Civil c = civil(secs);
        if (civil_secs(c) != secs) {
            FAILF("%lld did not round-trip", (long long)secs);
            return;
        }
        if (c.month < 1 || c.month > 12 || c.day < 1 || c.day > 31 || c.weekday > 6) {
            FAILF("%lld: %u-%u weekday %u", (long long)secs, c.month, c.day, c.weekday);
            return;
        }
        // Midnight is a new day: yesterday's weekday + 1.
        if (c.hour == 0) {
            if (have_day && c.weekday != (prev_day + 1) % 7) {
                FAILF("%lld: the weekday skipped", (long long)secs);
                return;
            }
            prev_day = c.weekday;
            have_day = true;
        }
        n++;
    }
    CHECK_EQ(n, (to - from) / step);
    CHECK(n > 200000);
}

/// The oracle is libc, through `date`: arithmetic that agrees with itself
/// proves nothing. One process for the whole list.
CASE(time_every_date_matches_what_libc_says)
{
    std::vector<i64> want = { 0,          -1,           1,          86399,       86400,
                              -86400,     951782400,    1709208000, -2203891200LL,
                              4102444800LL };
    // Every 97 days for sixty years: not all Mondays, not all January.
    for (i64 t = -946684800LL; t < 2000000000LL; t += 97 * 86400 + 3607)
        want.push_back(t);

    String input;
    for (i64 s : want) {
        char line[32];
        snprintf(line, sizeof(line), "@%lld\n", (long long)s);
        input += line;
    }

    String path = "/tmp/koru-time-cpp-in";
    FILE *in    = fopen(path.c_str(), "w");
    REQUIRE(in != nullptr);
    fwrite(input.data(), 1, input.size(), in);
    fclose(in);

    String cmd = "date -u -f " + path + " '+%Y %m %d %H %M %S %a' 2>/dev/null";
    FILE *p    = popen(cmd.c_str(), "r");
    if (!p) {
        FAILF("no date(1): the calendar was checked against nothing");
        return;
    }
    size_t i = 0;
    char line[128];
    while (fgets(line, sizeof(line), p)) {
        size_t n = strlen(line);
        while (n && (line[n - 1] == '\n' || line[n - 1] == '\r'))
            line[--n] = 0;
        if (i >= want.size()) {
            FAILF("date(1) answered more lines than it was asked");
            break;
        }
        Civil c = civil(want[i]);
        char got[128];
        snprintf(got, sizeof(got), "%d %02u %02u %02u %02u %02u %.*s", c.year, c.month, c.day,
                 c.hour, c.min, c.sec, int(TIME_DAYS[c.weekday].size()),
                 TIME_DAYS[c.weekday].data());
        if (strcmp(got, line) != 0)
            FAILF("%lld: [%s] != [%s]", (long long)want[i], got, line);
        i++;
    }
    int rc = pclose(p);
    if (rc != 0) {
        FAILF("no date(1): the calendar was checked against nothing");
        return;
    }
    CHECK_EQ(i, want.size());
    remove(path.c_str());
}

// ---------------------------------------------------------------------------
// The pure halves of the operation layer
// ---------------------------------------------------------------------------

CASE(ops_braams_open_flags_become_korus)
{
    CHECK_EQ(detail::open_flags(O_READ).value().first, KORU_O_RDONLY);
    CHECK_EQ(detail::open_flags(0).value().first, KORU_O_RDONLY);
    CHECK_EQ(detail::open_flags(O_WRITE).value().first, KORU_O_WRONLY);
    CHECK_EQ(detail::open_flags(O_READ | O_WRITE).value().first, KORU_O_RDWR);
    CHECK_EQ(detail::open_flags(O_WRITE | O_CREATE | O_TRUNC).value().first,
             KORU_O_WRONLY | KORU_O_CREAT | KORU_O_TRUNC);
    CHECK_EQ(detail::open_flags(O_WRITE | O_APPEND).value().first,
             KORU_O_WRONLY | KORU_O_APPEND);
    // A bit koru does not know, and one that cannot act.
    CHECK(!detail::open_flags(1u << 31).ok());
    CHECK(!detail::open_flags(O_WRITE | O_EXCL).ok());
    CHECK(detail::open_flags(O_WRITE | O_CREATE | O_EXCL).ok());
}

/// Braam's own wording: "who: what: why", and no empty field.
CASE(ops_a_diagnostic_reads_who_what_why)
{
    Error e = Error::from_errno(Errno(ENOENT));
    CHECK(detail::diag_line("wc", "a.txt", e) == "wc: a.txt: not found\n");
    CHECK(detail::diag_line("wc", "", e) == "wc: not found\n");
}

CASE(ops_a_path_joins_without_doubling_the_separator)
{
    CHECK(detail::join("/a", "b") == "/a/b");
    CHECK(detail::join("/a/", "b") == "/a/b");
    CHECK(detail::join("/", "b") == "/b");
}

CASE(ops_an_mtime_is_milliseconds_and_never_negative)
{
    CHECK_EQ(detail::ms_of(1, 500000000), 1500);
    CHECK_EQ(detail::ms_of(0, 0), 0);
    CHECK_EQ(detail::ms_of(-1, 0), 0); // a date before the epoch reports none
}

/// The three kinds Braam has, off koru's whole `S_IFMT`.
CASE(ops_every_file_type_lands_in_one_of_braams_three_kinds)
{
    CHECK(detail::kind_of_mode(KORU_S_IFDIR | 0755) == FileKind::Dir);
    CHECK(detail::kind_of_mode(KORU_S_IFLNK | 0777) == FileKind::Link);
    CHECK(detail::kind_of_mode(KORU_S_IFREG | 0644) == FileKind::File);
    CHECK(detail::kind_of_mode(KORU_S_IFIFO) == FileKind::File);
    CHECK(detail::kind_of_mode(KORU_S_IFCHR) == FileKind::File);
    CHECK(detail::kind_of_mode(KORU_S_IFSOCK) == FileKind::File);
}

/// `.` and `..` never reach a caller, and a short record stops the walk.
CASE(ops_a_dirent_run_parses_to_its_names)
{
    std::vector<u8> buf;
    struct {
        const char *name;
        u8 dtype;
    } rows[] = { { "a", KORU_DT_REG }, { ".", KORU_DT_DIR }, { "bb", KORU_DT_DIR } };

    for (const auto &r : rows) {
        size_t head   = sizeof(koru_dirent);
        size_t namelen = strlen(r.name);
        size_t reclen = (head + namelen + 1 + 7) & ~size_t(7);
        size_t at     = buf.size();
        buf.resize(at + reclen, 0);
        // The header by hand, as the kernel lays it out.
        koru_dirent d = {};
        d.ino         = 1;
        d.cookie      = 1;
        d.reclen      = u16(reclen);
        d.namelen     = u16(namelen);
        d.dtype       = r.dtype;
        memcpy(buf.data() + at, &d, head);
        memcpy(buf.data() + at + head, r.name, namelen);
    }

    std::vector<std::pair<String, u8>> out;
    detail::parse_dirents(Span<u8>(buf.data(), buf.size()), out);
    CHECK_EQ(out.size(), 2);
    if (out.size() == 2) {
        CHECK(out[0].first == "a" && out[0].second == KORU_DT_REG);
        CHECK(out[1].first == "bb" && out[1].second == KORU_DT_DIR);
    }

    // A truncated last record is dropped, not guessed at.
    out.clear();
    detail::parse_dirents(Span<u8>(buf.data(), buf.size() - 8), out);
    CHECK_EQ(out.size(), 1);
}

// ---------------------------------------------------------------------------
// UTF-8 and the buffer
// ---------------------------------------------------------------------------

namespace {

FileBuf filled(Str s, size_t cap)
{
    FileBuf b = FileBuf::with_capacity(cap);
    b.append(Span<u8>(reinterpret_cast<const u8 *>(s.data()), s.size()));
    return b;
}

bool held_is(const FileBuf &b, Str want)
{
    Span<u8> h = b.held();
    return h.size() == want.size() && memcmp(h.data(), want.data(), want.size()) == 0;
}

} // namespace

CASE(filebuf_held_shrinks_as_it_is_consumed_and_resets_when_it_empties)
{
    FileBuf b = filled("abcdef", 16);
    CHECK_EQ(b.size(), 6);
    CHECK(held_is(b, "abcdef"));
    b.consume(2);
    CHECK(held_is(b, "cdef"));
    // Consuming past the end takes what is there and no more.
    b.consume(99);
    CHECK(b.is_empty());
    CHECK_EQ(b.room(), 16); // and the block is whole again
}

CASE(filebuf_compact_moves_the_held_bytes_to_the_front)
{
    FileBuf b = filled("abcdef", 8);
    b.consume(4);
    CHECK_EQ(b.room(), 2);
    b.compact();
    CHECK(held_is(b, "ef"));
    CHECK_EQ(b.room(), 6);
}

CASE(filebuf_a_rune_comes_out_whole_or_not_at_all)
{
    FileBuf b = filled("a\xc3\xa9\xe2\x98\x83", 16);
    u32 c     = 0;
    CHECK(b.take(c) == RuneStep::Got && c == u32('a'));
    CHECK(b.take(c) == RuneStep::Got && c == 0xe9);
    CHECK(b.take(c) == RuneStep::Got && c == 0x2603);
    CHECK(b.take(c) == RuneStep::Need); // nothing left is not a rune

    // A sequence cut short asks for more rather than guessing.
    FileBuf p = filled("\xe2\x98", 16);
    CHECK(p.take(c) == RuneStep::Need);
    CHECK(p.append(Span<u8>(reinterpret_cast<const u8 *>("\x83"), 1)));
    CHECK(p.take(c) == RuneStep::Got && c == 0x2603);
}

/// Braam's rule: every malformed sequence is one byte and U+FFFD.
CASE(filebuf_every_malformed_sequence_is_one_byte_and_a_replacement)
{
    const char *bad[] = {
        "\x80",         // a stray continuation
        "\xff",         // a lead that cannot start one
        "\xc0\x80",     // overlong
        "\xed\xa0\x80", // a surrogate
        "\xe2\x28\xa1", // a missing continuation
        "\xf5\x80\x80", // past U+10FFFF
    };
    for (const char *s : bad) {
        FileBuf b = filled(Str(s, strlen(s)), 16);
        size_t was = b.size();
        u32 c      = 0;
        if (b.take(c) != RuneStep::Got || c != RUNE_REPLACEMENT)
            FAILF("%s did not decode to a replacement", s);
        if (b.size() != was - 1)
            FAILF("%s took more than one byte", s);
    }
}

CASE(filebuf_take_broken_is_one_byte_and_a_replacement_even_at_the_end)
{
    FileBuf b = filled("\xe2", 16);
    u32 c     = 0;
    CHECK(b.take(c) == RuneStep::Need);
    CHECK_EQ(b.take_broken(), RUNE_REPLACEMENT);
    CHECK(b.is_empty());
    // An empty buffer still answers, and consumes nothing.
    CHECK_EQ(b.take_broken(), RUNE_REPLACEMENT);
}

CASE(filebuf_unget_goes_in_front_of_what_is_held)
{
    FileBuf b = filled("bc", 16);
    CHECK(b.unget(u32('a')));
    CHECK(held_is(b, "abc"));
    u32 c = 0;
    CHECK(b.take(c) == RuneStep::Got && c == u32('a'));

    // A multi-byte rune, and one that will not fit.
    CHECK(b.unget(0x2603));
    CHECK(held_is(b, "\xe2\x98\x83" "bc"));
    FileBuf tight = filled("xy", 3);
    CHECK(!tight.unget(0x2603)); // no room in front or behind
    CHECK(held_is(tight, "xy")); // and nothing was disturbed
}

CASE(filebuf_a_line_is_taken_whole_or_left_as_a_fragment)
{
    FileBuf b = filled("one\ntwo", 16);
    String out;
    CHECK(b.take_line(out, false) == LineStep::Done);
    CHECK(out == "one");
    CHECK(!b.has_line()); // what is left has no newline

    out.clear();
    CHECK(b.take_line(out, false) == LineStep::Need);
    CHECK(out == "two");
    CHECK(b.is_empty()); // the fragment was consumed too

    FileBuf k = filled("one\n", 16);
    out.clear();
    CHECK(k.take_line(out, true) == LineStep::Done);
    CHECK(out == "one\n"); // keep_nl keeps it
}

CASE(filebuf_append_refuses_what_will_not_fit_rather_than_truncating)
{
    FileBuf b = FileBuf::with_capacity(4);
    CHECK(b.append(Span<u8>(reinterpret_cast<const u8 *>("abc"), 3)));
    CHECK(!b.append(Span<u8>(reinterpret_cast<const u8 *>("de"), 2)));
    CHECK(held_is(b, "abc"));
    CHECK_EQ(b.append_rune(u32('d')), 1);
    CHECK_EQ(b.append_rune(u32('e')), 0); // full
    CHECK(held_is(b, "abcd"));

    FileBuf w = FileBuf::with_capacity(4);
    CHECK_EQ(w.append_rune(0x2603), 3);
    CHECK_EQ(w.append_rune(0x2603), 0); // three more would not fit
}

/// An `Input` chunk moves in whole, with no room behind it.
CASE(filebuf_an_adopted_chunk_is_held_entirely_and_has_no_room)
{
    FileBuf b;
    b.adopt("hello");
    CHECK(held_is(b, "hello"));
    CHECK_EQ(b.room(), 0);
}

CASE(filebuf_regrow_keeps_what_is_held)
{
    FileBuf b = filled("abcdef", 8);
    b.consume(2);
    CHECK(b.regrow(32));
    CHECK(held_is(b, "cdef"));
    CHECK_EQ(b.room(), 28);
    CHECK(!b.regrow(16)); // already bigger
}

/// Every codepoint encodes and decodes back to itself, surrogates aside.
CASE(filebuf_utf8_round_trips_every_codepoint)
{
    for (u32 c = 0; c <= 0x10ffff; c++) {
        if (c >= 0xd800 && c <= 0xdfff)
            continue; // not a codepoint a stream may carry
        u8 e[4];
        size_t n = utf8_encode(c, e);
        if (utf8_decode(Span<u8>(e, n)) != n) {
            FAILF("U+%04X encodes to %zu bytes the decoder refuses", c, n);
            return;
        }
        if (utf8_rune(Span<u8>(e, n)) != c) {
            FAILF("U+%04X did not round-trip", c);
            return;
        }
    }
}
