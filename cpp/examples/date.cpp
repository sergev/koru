// SPDX-License-Identifier: MIT
//
// T47's done test: Braam's `src/cmd/date.cpp`, unchanged but for the include
// line. The program shell end to end — the two usage helpers, the calendar and
// the name tables — because which stream and which status a usage block goes
// to is a program's question and cannot be asked of a library.
//
// `rust/runtime/examples/date.rs` is the same program in the other binding,
// and scripts/cpp.sh runs both with the same redirections.

#include <koru/braam.hpp>

#include <cstdio>

namespace {

const char *USAGE = "Usage:\n"
                    "    date [-u]\n"
                    "Options:\n"
                    "    -u    UTC, rather than the local time\n";

} // namespace

Task<Result<i32>> koru_main(Args args)
{
    if (help_asked(args))
        co_return co_await usage_asked(USAGE);

    bool utc = args.size() > 1 && args[1] == "-u";
    if (args.size() > 2 || (args.size() == 2 && !utc))
        co_return co_await usage_error(USAGE);

    Result<Clock> now = co_await clock_now();
    if (!now.ok()) {
        if (now.error() == Kind::Cancelled)
            co_return 130;
        co_await errln("date", "", now.error());
        co_return 1;
    }

    i32 tz  = utc ? 0 : now.value().tz_min;
    Civil c = civil(i64(now.value().epoch_ms / 1000) + i64(tz) * 60);
    u32 off = u32(tz < 0 ? -tz : tz);

    char line[128];
    snprintf(line, sizeof(line), "%.*s %.*s %02u %02u:%02u:%02u %c%02u%02u %d\n",
             int(TIME_DAYS[c.weekday].size()), TIME_DAYS[c.weekday].data(),
             int(TIME_MONTHS[c.month - 1].size()), TIME_MONTHS[c.month - 1].data(), c.day, c.hour,
             c.min, c.sec, tz < 0 ? '-' : '+', off / 60, off % 60, c.year);

    co_return (co_await write_all(out_fd(), line)).ok() ? 0 : 1;
}
