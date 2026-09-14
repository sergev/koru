// SPDX-License-Identifier: MIT

#include <koru/time.hpp>

namespace koru {

const Str TIME_MONTHS[12] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun",
                              "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };

const Str TIME_DAYS[7] = { "Thu", "Fri", "Sat", "Sun", "Mon", "Tue", "Wed" };

namespace {

/// C++'s `/` and `%` truncate towards zero; this arithmetic needs the floor.
i64 div_floor(i64 a, i64 b)
{
    i64 q = a / b;
    return (a % b != 0 && ((a < 0) != (b < 0))) ? q - 1 : q;
}

i64 mod_floor(i64 a, i64 b)
{
    return a - div_floor(a, b) * b;
}

} // namespace

/// The branch-free `civil_from_days`, by way of an era of 400 years.
Civil civil(i64 secs)
{
    i64 days = div_floor(secs, 86400);
    i64 rem  = mod_floor(secs, 86400);

    i64 z     = days + 719468;
    i64 era   = (z >= 0 ? z : z - 146096) / 146097;
    i64 doe   = z - era * 146097;                                   // 0..=146096
    i64 yoe   = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365; // 0..=399
    i64 doy   = doe - (365 * yoe + yoe / 4 - yoe / 100);
    i64 mp    = (5 * doy + 2) / 153;
    i64 month = mp < 10 ? mp + 3 : mp - 9;

    Civil c;
    c.year    = i32(yoe + era * 400) + (month <= 2 ? 1 : 0);
    c.month   = u32(month);
    c.day     = u32(doy - (153 * mp + 2) / 5 + 1);
    c.hour    = u32(rem / 3600);
    c.min     = u32((rem / 60) % 60);
    c.sec     = u32(rem % 60);
    c.weekday = u32(mod_floor(days, 7));
    return c;
}

i64 civil_secs(const Civil &c)
{
    // Carry the month into the year: month 0 is December of the year before.
    i64 mz  = i64(c.month) - 1;
    i64 adj = mz >= 0 ? mz / 12 : -((-mz + 11) / 12);
    i64 y   = i64(c.year) + adj;
    i64 m   = mz - adj * 12 + 1; // 1..=12

    y -= m <= 2 ? 1 : 0;
    i64 era  = (y >= 0 ? y : y - 399) / 400;
    i64 yoe  = y - era * 400; // 0..=399
    i64 doy  = (153 * (m > 2 ? m - 3 : m + 9) + 2) / 5 + i64(c.day) - 1;
    i64 doe  = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    i64 days = era * 146097 + doe - 719468;

    return days * 86400 + i64(c.hour) * 3600 + i64(c.min) * 60 + i64(c.sec);
}

} // namespace koru
