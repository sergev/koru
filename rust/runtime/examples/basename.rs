// SPDX-License-Identifier: MIT

//! Braam's `src/cmd/basename.cpp`, in Rust. The option parser, the path
//! helpers and nothing else: no ring call but the one write at the end.

use koru::{Args, Kind, OptParse, Opts, Str, help_asked, usage_asked, usage_error, write_all};

const USAGE: &str = "Usage:
    basename <path> [<suffix>]
    basename [-a] [-s <suffix>] <path>...
Options:
    -a    every operand is a path, none of them a suffix
    -s    strip this suffix, and imply -a
";

/// Text, not a path: nothing here opens anything. A name that is only the
/// suffix keeps it, rather than coming out empty.
fn base_of<'a>(mut p: Str<'a>, suffix: Str<'_>) -> Str<'a> {
    if p.is_empty() {
        return p;
    }
    while p.len() > 1 && p.ends_with('/') {
        p = &p[..p.len() - 1];
    }
    if p == "/" {
        return p;
    }

    let name = match p.rfind('/') {
        Some(i) => &p[i + 1..],
        None => p,
    };
    if !suffix.is_empty() && name != suffix && name.ends_with(suffix) {
        return &name[..name.len() - suffix.len()];
    }
    name
}

#[koru::main]
async fn main(args: Args) -> i32 {
    if args.size() == 1 || help_asked(&args) {
        return usage_asked(USAGE).await;
    }

    let mut all = false;
    let mut suffix = "";
    let mut p = OptParse::new(
        &args,
        Opts {
            flags: "a",
            valued: "s",
        },
    );
    loop {
        match p.next() {
            Err(e) => {
                let why = if e.error.is(Kind::NotFound) {
                    "option requires an argument -- "
                } else {
                    "illegal option -- "
                };
                let line = format!("basename: {}{}\n", why, e.name);
                write_all(koru::stderr(), &line).await.ok();
                return usage_error(USAGE).await;
            }
            Ok(None) => break,
            Ok(Some(o)) => {
                if o.name == 's' {
                    suffix = o.value;
                }
                all = true; // -s implies -a
            }
        }
    }

    // Without either flag a second operand is the suffix, and there is no third.
    let rest = p.rest();
    let mut take = rest.size();
    if take == 0 || (!all && take > 2) {
        return usage_error(USAGE).await;
    }
    if !all && take == 2 {
        suffix = &rest[1];
        take = 1;
    }

    let mut out = String::new();
    for i in 0..take {
        out.push_str(base_of(&rest[i], suffix));
        out.push('\n');
    }
    i32::from(write_all(koru::stdout(), &out).await.is_err())
}
