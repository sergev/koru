// SPDX-License-Identifier: MIT

//! Braam's `proc/time.h`: the calendar, shared by `date` and by `ls -l`.
//! Pure — no syscall, no ring. The clock and the zone are the caller's, from
//! [`crate::clock_now`].

use crate::vocab::Str;

/// `month` and `day` are 1-based, `weekday` indexes [`TIME_DAYS`].
#[derive(Copy, Clone, PartialEq, Eq, Debug, Default)]
pub struct Civil {
    pub year: i32,
    pub month: u32,
    pub day: u32,
    pub hour: u32,
    pub min: u32,
    pub sec: u32,
    pub weekday: u32,
}

pub const TIME_MONTHS: [Str<'static>; 12] = [
    "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec",
];

/// 1970-01-01 was a Thursday, and the weekday is days mod 7 from there.
pub const TIME_DAYS: [Str<'static>; 7] = ["Thu", "Fri", "Sat", "Sun", "Mon", "Tue", "Wed"];

/// Seconds since 1970-01-01 to a calendar date. Negative seconds work.
///
/// The branch-free `civil_from_days`, by way of an era of 400 years. All
/// `i64`, not Braam's `u64`: a Rust subtraction below zero panics.
pub fn civil(secs: i64) -> Civil {
    let days = secs.div_euclid(86400);
    let rem = secs.rem_euclid(86400);

    let z = days + 719_468;
    let era = if z >= 0 { z } else { z - 146_096 } / 146_097;
    let doe = z - era * 146_097; // 0..=146096
    let yoe = (doe - doe / 1460 + doe / 36524 - doe / 146_096) / 365; // 0..=399
    let doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    let mp = (5 * doy + 2) / 153;
    let month = if mp < 10 { mp + 3 } else { mp - 9 };

    Civil {
        year: (yoe + era * 400) as i32 + i32::from(month <= 2),
        month: month as u32,
        day: (doy - (153 * mp + 2) / 5 + 1) as u32,
        hour: (rem / 3600) as u32,
        min: ((rem / 60) % 60) as u32,
        sec: (rem % 60) as u32,
        weekday: days.rem_euclid(7) as u32,
    }
}

/// The inverse. Fields outside their ranges normalise and `weekday` is
/// ignored, which is `mktime`'s contract.
pub fn civil_secs(c: Civil) -> i64 {
    // Carry the month into the year: month 0 is December of the year before.
    let mz = i64::from(c.month) - 1;
    let adj = if mz >= 0 { mz / 12 } else { -((-mz + 11) / 12) };
    let mut y = i64::from(c.year) + adj;
    let m = mz - adj * 12 + 1; // 1..=12

    y -= i64::from(m <= 2);
    let era = if y >= 0 { y } else { y - 399 } / 400;
    let yoe = y - era * 400; // 0..=399
    let doy = (153 * if m > 2 { m - 3 } else { m + 9 } + 2) / 5 + i64::from(c.day) - 1;
    let doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    let days = era * 146_097 + doe - 719_468;

    days * 86400 + i64::from(c.hour) * 3600 + i64::from(c.min) * 60 + i64::from(c.sec)
}

#[cfg(test)]
mod tests {
    use super::*;

    /// Braam's `check`: the fields, the day name, and the round trip.
    fn check(secs: i64, want: (i32, u32, u32, u32, u32, u32), day: Str<'_>) {
        let c = civil(secs);
        assert_eq!(
            (c.year, c.month, c.day, c.hour, c.min, c.sec),
            want,
            "{secs}"
        );
        assert_eq!(TIME_DAYS[c.weekday as usize], day, "{secs}");
        assert_eq!(civil_secs(c), secs, "{secs} did not round-trip");
    }

    /// Braam's `test_time.cpp`, vector for vector.
    #[test]
    fn the_epoch_and_the_days_either_side_of_it() {
        check(0, (1970, 1, 1, 0, 0, 0), "Thu");
        check(86399, (1970, 1, 1, 23, 59, 59), "Thu");
        check(86400, (1970, 1, 2, 0, 0, 0), "Fri");
    }

    #[test]
    fn a_leap_day_and_the_day_after_it() {
        check(1_709_208_000, (2024, 2, 29, 12, 0, 0), "Thu");
        check(1_709_294_400, (2024, 3, 1, 12, 0, 0), "Fri");
    }

    /// 1900 is not a leap year and 2000 is — the two the era arithmetic exists
    /// to get right without a table.
    #[test]
    fn the_century_rule_both_ways() {
        check(-2_203_891_200, (1900, 3, 1, 0, 0, 0), "Thu");
        check(951_782_400, (2000, 2, 29, 0, 0, 0), "Tue");
    }

    /// Before the epoch, where the seconds-in-day remainder goes negative.
    #[test]
    fn before_the_epoch() {
        check(-1, (1969, 12, 31, 23, 59, 59), "Wed");
        check(-86400, (1969, 12, 31, 0, 0, 0), "Wed");
    }

    #[test]
    fn the_name_tables_are_braams() {
        assert_eq!(TIME_MONTHS[0], "Jan");
        assert_eq!(TIME_MONTHS[11], "Dec");
        assert_eq!(TIME_DAYS[0], "Thu");
        assert_eq!(TIME_DAYS[6], "Wed");
    }

    /// Out-of-range fields normalise, which is what `mktime` callers rely on.
    #[test]
    fn fields_outside_their_ranges_normalise() {
        let at = |y, mo, d, h, mi, s| {
            civil_secs(Civil {
                year: y,
                month: mo,
                day: d,
                hour: h,
                min: mi,
                sec: s,
                weekday: 0,
            })
        };
        assert_eq!(at(1970, 13, 1, 0, 0, 0), at(1971, 1, 1, 0, 0, 0));
        assert_eq!(at(1970, 0, 1, 0, 0, 0), at(1969, 12, 1, 0, 0, 0));
        assert_eq!(at(1970, 1, 32, 0, 0, 0), at(1970, 2, 1, 0, 0, 0));
        assert_eq!(at(1970, 1, 1, 25, 0, 0), 25 * 3600);
        assert_eq!(at(2024, 2, 30, 0, 0, 0), at(2024, 3, 1, 0, 0, 0));
        // Day 0 is the last of the month before; Braam's `u64` would wrap.
        assert_eq!(at(2024, 3, 0, 0, 0, 0), at(2024, 2, 29, 0, 0, 0));
    }

    /// Every six hours over a century and a half, both sides of the epoch.
    /// The weekday advances by one a day across all of it, which no single
    /// vector can say.
    #[test]
    fn every_date_over_a_century_and_a_half_round_trips() {
        let step = 6 * 3600;
        let from = civil_secs(Civil {
            year: 1900,
            month: 1,
            day: 1,
            ..Civil::default()
        });
        let to = civil_secs(Civil {
            year: 2050,
            month: 1,
            day: 1,
            ..Civil::default()
        });

        let mut prev_day = None;
        let mut n = 0;
        let mut secs = from;
        while secs < to {
            let c = civil(secs);
            assert_eq!(civil_secs(c), secs, "{secs}");
            assert!((1..=12).contains(&c.month), "{secs}: month {}", c.month);
            assert!((1..=31).contains(&c.day), "{secs}: day {}", c.day);
            assert!(c.weekday < 7, "{secs}: weekday {}", c.weekday);

            // Midnight is a new day: yesterday's weekday + 1.
            if c.hour == 0 {
                if let Some(p) = prev_day {
                    assert_eq!(c.weekday, (p + 1) % 7, "{secs}: the weekday skipped");
                }
                prev_day = Some(c.weekday);
            }
            secs += step;
            n += 1;
        }
        assert_eq!(n, (to - from) / step);
        assert!(n > 200_000, "the span shrank to {n} samples");
    }

    /// The oracle is libc, through `date`: arithmetic that agrees with itself
    /// proves nothing. One process for the whole list.
    #[test]
    fn every_date_matches_what_libc_says() {
        let mut want: Vec<i64> = vec![
            0,
            -1,
            1,
            86399,
            86400,
            -86400,
            951_782_400,
            1_709_208_000,
            -2_203_891_200,
            4_102_444_800, // 2100-01-01, the next non-leap century
        ];
        // Every 97 days for sixty years: not all Mondays, not all January.
        let mut t = -946_684_800; // 1940-01-01
        while t < 2_000_000_000 {
            want.push(t);
            t += 97 * 86400 + 3607;
        }

        let input: String = want.iter().map(|s| format!("@{s}\n")).collect();
        let Some(out) = date_utc(&input) else {
            panic!("no date(1): the calendar was checked against nothing");
        };
        let lines: Vec<&str> = out.lines().collect();
        assert_eq!(
            lines.len(),
            want.len(),
            "date(1) answered a different count"
        );

        for (&secs, line) in want.iter().zip(lines) {
            let c = civil(secs);
            let got = format!(
                "{} {:02} {:02} {:02} {:02} {:02} {}",
                c.year, c.month, c.day, c.hour, c.min, c.sec, TIME_DAYS[c.weekday as usize]
            );
            assert_eq!(got, line, "{secs}");
        }
    }

    /// `date -u -f -`, which reads `@<secs>` a line at a time.
    fn date_utc(input: &str) -> Option<String> {
        use std::io::Write;
        use std::process::{Command, Stdio};

        let mut child = Command::new("date")
            .args(["-u", "-f", "-", "+%Y %m %d %H %M %S %a"])
            .stdin(Stdio::piped())
            .stdout(Stdio::piped())
            .stderr(Stdio::null())
            .spawn()
            .ok()?;
        child.stdin.take()?.write_all(input.as_bytes()).ok()?;
        let out = child.wait_with_output().ok()?;
        out.status
            .success()
            .then(|| String::from_utf8_lossy(&out.stdout).into_owned())
    }
}
