// SPDX-License-Identifier: MIT
//
// Braam's `proc/time.h`: the calendar, shared by `date` and by `ls -l`. Pure —
// no syscall, no ring. The clock and the zone are the caller's, from
// [`clock_now`]. The C++ half of rust/runtime/src/time.rs.

#ifndef KORU_TIME_HPP
#define KORU_TIME_HPP

#include <koru/vocab.hpp>

namespace koru {

/// `month` and `day` are 1-based, `weekday` indexes [`TIME_DAYS`].
struct Civil {
    i32 year    = 0;
    u32 month   = 0;
    u32 day     = 0;
    u32 hour    = 0;
    u32 min     = 0;
    u32 sec     = 0;
    u32 weekday = 0;
};

extern const Str TIME_MONTHS[12];

/// 1970-01-01 was a Thursday, and the weekday is days mod 7 from there.
extern const Str TIME_DAYS[7];

/// Seconds since 1970-01-01 to a calendar date. Negative seconds work.
Civil civil(i64 secs);

/// The inverse. Fields outside their ranges normalise and `weekday` is
/// ignored, which is `mktime`'s contract.
i64 civil_secs(const Civil &c);

} // namespace koru

#endif // KORU_TIME_HPP
