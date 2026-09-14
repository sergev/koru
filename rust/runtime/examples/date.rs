// SPDX-License-Identifier: MIT

//! Braam's `src/cmd/date.cpp`, in Rust: the program shell end to end — the
//! two usage helpers, the calendar and the name tables.

use koru::{Args, Kind, TIME_DAYS, TIME_MONTHS, civil, clock_now, errln, help_asked, usage_asked};
use koru::{usage_error, write_all};

const USAGE: &str = "Usage:
    date [-u]
Options:
    -u    UTC, rather than the local time
";

#[koru::main]
async fn main(args: Args) -> i32 {
    if help_asked(&args) {
        return usage_asked(USAGE).await;
    }

    let utc = args.size() > 1 && &args[1] == "-u";
    if args.size() > 2 || (args.size() == 2 && !utc) {
        return usage_error(USAGE).await;
    }

    let now = match clock_now().await {
        Ok(c) => c,
        Err(e) if e.is(Kind::Cancelled) => return 130,
        Err(e) => {
            errln("date", "", e).await;
            return 1;
        }
    };

    let tz = if utc { 0 } else { now.tz_min };
    let c = civil((now.epoch_ms / 1000) as i64 + i64::from(tz) * 60);
    let off = tz.unsigned_abs();
    let line = format!(
        "{} {} {:02} {:02}:{:02}:{:02} {}{:02}{:02} {}\n",
        TIME_DAYS[c.weekday as usize],
        TIME_MONTHS[c.month as usize - 1],
        c.day,
        c.hour,
        c.min,
        c.sec,
        if tz < 0 { '-' } else { '+' },
        off / 60,
        off % 60,
        c.year
    );

    i32::from(write_all(koru::stdout(), &line).await.is_err())
}
