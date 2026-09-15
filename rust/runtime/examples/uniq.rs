// SPDX-License-Identifier: MIT

//! Braam's `src/cmd/uniq.cpp`, in Rust. Adjacent lines compared, a run at a
//! time: the line held is the run's first, and the count is written before it
//! under `-c`.

use koru::write_all;
use koru::{Args, Input, Kind, OptParse, Opts, Str, help_asked, usage_asked, usage_error};

const USAGE: &str = "Usage:
    uniq [-cdu] [-f <n>] [-s <n>] [<file>...]
Options:
    -c    prefix each line with how many times it ran
    -d    print only the lines that repeated
    -u    print only the lines that did not
    -f    ignore the first <n> blank-delimited fields
    -s    ignore <n> further bytes
";

/// How much output is gathered before a write.
const UNIQ_ROWS: usize = 4096;

#[derive(Default)]
struct Uniqer {
    count: bool,
    dups: bool,
    singles: bool,
    fields: u32,
    bytes: u32,
}

/// The run being counted: its first line, which is what prints, and how long
/// it has run for.
#[derive(Default)]
struct Runs {
    held: Option<String>,
    run: u32,
}

/// Where the compared part begins: past `fields` fields, then past `bytes`
/// bytes. Bytes, not characters — a Cyrillic letter is two of them.
fn compared(s: Str<'_>, fields: u32, bytes: u32) -> &[u8] {
    let b = s.as_bytes();
    let blank = |c: u8| c == b' ' || c == b'\t';
    let mut i = 0;
    for _ in 0..fields {
        while i < b.len() && blank(b[i]) {
            i += 1;
        }
        while i < b.len() && !blank(b[i]) {
            i += 1;
        }
    }
    let n = bytes as usize;
    i = if n > b.len() - i { b.len() } else { i + n };
    &b[i..]
}

/// A group's line into the pending output, or nothing when the flags exclude
/// it.
fn emit(u: &Uniqer, g: &Runs, rows: &mut String) {
    let Some(held) = g.held.as_deref() else {
        return;
    };
    if (u.dups && g.run < 2) || (u.singles && g.run != 1) {
        return;
    }
    if u.count {
        rows.push_str(&format!("{:>4} ", g.run));
    }
    rows.push_str(held);
    rows.push('\n');
}

/// One line: the run it continues, or the one it ends and the one it begins.
fn step(u: &Uniqer, g: &mut Runs, line: Str<'_>, rows: &mut String) {
    if let Some(held) = g.held.as_deref() {
        if compared(held, u.fields, u.bytes) == compared(line, u.fields, u.bytes) {
            g.run += 1;
            return;
        }
        emit(u, g, rows);
    }
    g.held = Some(line.to_string());
    g.run = 1;
}

async fn complain(why: Str<'_>, word: Str<'_>) -> i32 {
    let line = if word.is_empty() {
        format!("uniq: {why}\n")
    } else {
        format!("uniq: {why}: {word}\n")
    };
    write_all(koru::stderr(), &line).await.ok();
    usage_error(USAGE).await
}

#[koru::main]
async fn main(args: Args) -> i32 {
    if help_asked(&args) {
        return usage_asked(USAGE).await;
    }

    let mut u = Uniqer::default();
    let mut p = OptParse::new(
        &args,
        Opts {
            flags: "cdu",
            valued: "fs",
        },
    );
    loop {
        let o = match p.next() {
            Err(e) => {
                let why = if e.error.is(Kind::NotFound) {
                    "needs a value"
                } else {
                    "bad option"
                };
                return complain(why, &e.name.to_string()).await;
            }
            Ok(None) => break,
            Ok(Some(o)) => o,
        };
        match o.name {
            'c' => u.count = true,
            'd' => u.dups = true,
            'u' => u.singles = true,
            _ => {
                let Ok(n) = o.value.parse::<u32>() else {
                    return complain("not a count", o.value).await;
                };
                if o.name == 'f' {
                    u.fields = n;
                } else {
                    u.bytes = n;
                }
            }
        }
    }

    let mut files = Input::new(p.rest(), koru::stdin(), "uniq");
    let mut rows = String::new();
    let mut g = Runs::default();
    let mut pending = String::new();

    // Split here rather than through a line reader: this program holds no
    // `File`, and a `File`'s bytes buy it nothing.
    loop {
        let chunk = match files.read().await {
            Ok(c) => c,
            Err(e) if e.is(Kind::Closed) => break,
            Err(e) => return if e.is(Kind::Cancelled) { 130 } else { 1 },
        };
        let Ok(text) = String::from_utf8(chunk) else {
            return 1;
        };

        let mut s: &str = &text;
        while !s.is_empty() {
            let Some(i) = s.find('\n') else {
                pending.push_str(s);
                break;
            };
            if pending.is_empty() {
                step(&u, &mut g, &s[..i], &mut rows);
            } else {
                pending.push_str(&s[..i]);
                let whole = std::mem::take(&mut pending);
                step(&u, &mut g, &whole, &mut rows);
            }

            s = &s[i + 1..];
            if rows.len() >= UNIQ_ROWS {
                match write_all(koru::stdout(), &rows).await {
                    Ok(()) => rows.clear(),
                    Err(e) => return if e.is(Kind::Cancelled) { 130 } else { 1 },
                }
            }
        }
    }

    // A final fragment with no newline is a line, and the last run is written
    // here either way.
    if !pending.is_empty() {
        let whole = std::mem::take(&mut pending);
        step(&u, &mut g, &whole, &mut rows);
    }
    emit(&u, &g, &mut rows);

    if !rows.is_empty() {
        if let Err(e) = write_all(koru::stdout(), &rows).await {
            return if e.is(Kind::Cancelled) { 130 } else { 1 };
        }
    }
    0
}
