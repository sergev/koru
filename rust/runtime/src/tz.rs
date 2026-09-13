// SPDX-License-Identifier: MIT

//! The host's offset from UTC, for `clock_now`'s `tz_min`.
//!
//! Braam's kernel asks the browser. There is no koru opcode for it and no std
//! API either, so this reads `/etc/localtime`, which is a TZif file (RFC 8536).
//! Everything unreadable or unparseable is 0, which is what UTC looks like.

const MAGIC: &[u8; 4] = b"TZif";

/// Minutes east of UTC in effect at `epoch_sec`.
pub fn local_offset_min(epoch_sec: i64) -> i32 {
    zone_file()
        .and_then(|buf| offset_at(&buf, epoch_sec))
        .map_or(0, |s| s / 60)
}

/// `$TZ` where it names a zone, else the system's. A POSIX `TZ` *string* names
/// no file, and then the system zone is the better of the two wrong answers.
fn zone_file() -> Option<Vec<u8>> {
    if let Ok(tz) = std::env::var("TZ") {
        let tz = tz.strip_prefix(':').unwrap_or(&tz);
        // Never upward: a TZ of "../../etc/shadow" must name nothing.
        if !tz.is_empty() && !tz.contains("..") {
            let path = if tz.starts_with('/') {
                tz.to_string()
            } else {
                format!("/usr/share/zoneinfo/{tz}")
            };
            if let Ok(buf) = std::fs::read(path) {
                return Some(buf);
            }
        }
    }
    std::fs::read("/etc/localtime").ok()
}

/// Seconds east of UTC, out of one TZif image.
fn offset_at(buf: &[u8], now: i64) -> Option<i32> {
    let v1 = Header::parse(buf, 0)?;
    // A version 2 or 3 file repeats everything with 64-bit times; the 32-bit
    // block ahead of it exists only for readers that stop at version 1.
    let (h, at) = if buf.get(4).is_some_and(|&v| v >= b'2') {
        let after = 44 + v1.block_len(4);
        (Header::parse(buf, after)?, after + 44)
    } else {
        (v1, 44)
    };

    let ts = if h.wide { 8 } else { 4 };
    let times = buf.get(at..at + h.timecnt * ts)?;
    let types = buf.get(at + h.timecnt * ts..at + h.timecnt * (ts + 1))?;
    let infos = at + h.timecnt * (ts + 1);

    // The last transition at or before `now`; before the first, the first
    // non-DST type, which is what localtime(3) falls back to.
    let mut which = None;
    for i in 0..h.timecnt {
        let t = if h.wide {
            i64::from_be_bytes(times[i * 8..i * 8 + 8].try_into().ok()?)
        } else {
            i64::from(i32::from_be_bytes(times[i * 4..i * 4 + 4].try_into().ok()?))
        };
        if t > now {
            break;
        }
        which = Some(types[i] as usize);
    }
    let which = match which {
        Some(i) => i,
        None => (0..h.typecnt)
            .find(|&i| buf[infos + i * 6 + 4] == 0)
            .unwrap_or(0),
    };
    if which >= h.typecnt {
        return None;
    }
    let off = buf.get(infos + which * 6..infos + which * 6 + 4)?;
    Some(i32::from_be_bytes(off.try_into().ok()?))
}

struct Header {
    wide: bool,
    isutcnt: usize,
    isstdcnt: usize,
    leapcnt: usize,
    timecnt: usize,
    typecnt: usize,
    charcnt: usize,
}

impl Header {
    fn parse(buf: &[u8], at: usize) -> Option<Header> {
        let h = buf.get(at..at + 44)?;
        if &h[..4] != MAGIC {
            return None;
        }
        let n = |i: usize| -> usize {
            u32::from_be_bytes(h[20 + i * 4..24 + i * 4].try_into().unwrap()) as usize
        };
        let out = Header {
            wide: h[4] >= b'2',
            isutcnt: n(0),
            isstdcnt: n(1),
            leapcnt: n(2),
            timecnt: n(3),
            typecnt: n(4),
            charcnt: n(5),
        };
        // A file with no types says nothing about any time.
        if out.typecnt == 0 {
            return None;
        }
        Some(out)
    }

    /// One data block, with `ts`-byte transition and leap-second times.
    fn block_len(&self, ts: usize) -> usize {
        self.timecnt * (ts + 1)
            + self.typecnt * 6
            + self.charcnt
            + self.leapcnt * (ts + 4)
            + self.isstdcnt
            + self.isutcnt
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    /// A version 1 image: one type, no transitions.
    fn v1(utoff: i32, isdst: u8) -> Vec<u8> {
        let mut b = Vec::new();
        b.extend_from_slice(MAGIC);
        b.push(0); // version 1
        b.extend_from_slice(&[0u8; 15]);
        for n in [0u32, 0, 0, 0, 1, 1] {
            b.extend_from_slice(&n.to_be_bytes()); // isut, isstd, leap, time, type, char
        }
        b.extend_from_slice(&utoff.to_be_bytes());
        b.push(isdst);
        b.push(0); // designation index
        b.push(0); // the one designation byte
        b
    }

    #[test]
    fn a_fixed_zone_reports_its_own_offset() {
        assert_eq!(offset_at(&v1(0, 0), 0), Some(0));
        assert_eq!(offset_at(&v1(3600, 0), 0), Some(3600));
        assert_eq!(offset_at(&v1(-18000, 0), 1_700_000_000), Some(-18000));
    }

    #[test]
    fn anything_that_is_not_a_tzif_image_says_nothing() {
        assert_eq!(offset_at(b"", 0), None);
        assert_eq!(offset_at(b"not a zone file at all, really", 0), None);
        let mut short = v1(3600, 0);
        short.truncate(40);
        assert_eq!(offset_at(&short, 0), None);
    }

    /// The oracle is the host's own libc, through `date`: a reader that agrees
    /// with itself proves nothing. Whole, half and quarter-hour offsets, and
    /// two zones whose summer time moves.
    #[test]
    fn every_offset_matches_what_libc_says() {
        let now = std::time::SystemTime::now()
            .duration_since(std::time::UNIX_EPOCH)
            .map_or(0, |d| d.as_secs() as i64);
        let mut checked = 0;

        for z in [
            "UTC",
            "America/Los_Angeles",
            "Europe/Moscow",
            "Asia/Kolkata",
            "Pacific/Chatham",
            "Australia/Sydney",
        ] {
            let Ok(buf) = std::fs::read(format!("/usr/share/zoneinfo/{z}")) else {
                continue;
            };
            let Some(want) = libc_offset_min(z) else {
                continue;
            };
            let got = offset_at(&buf, now).map(|s| s / 60);
            assert_eq!(got, Some(want), "{z}");
            checked += 1;
        }
        assert!(checked > 0, "no zone file and no date(1): nothing was read");
    }

    /// `date +%z` under `TZ`, in minutes east.
    fn libc_offset_min(zone: &str) -> Option<i32> {
        let out = std::process::Command::new("date")
            .env("TZ", zone)
            .arg("+%z")
            .output()
            .ok()?;
        let z = String::from_utf8_lossy(&out.stdout).trim().to_string();
        if z.len() != 5 {
            return None;
        }
        let sign = if z.starts_with('-') { -1 } else { 1 };
        let hh: i32 = z[1..3].parse().ok()?;
        let mm: i32 = z[3..5].parse().ok()?;
        Some(sign * (hh * 60 + mm))
    }
}
