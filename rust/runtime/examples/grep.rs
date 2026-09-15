// SPDX-License-Identifier: MIT

//! Braam's `src/cmd/grep.cpp`, in Rust. Plain substring search: there is no
//! regular-expression engine, and the usage line says so.

use koru::{Args, File, Input, Kind, help_asked, usage_asked, usage_error};

const USAGE: &str = "Usage:
    grep [-i] [-v] <text> [<file>...]
Options:
    -i    ignore case
    -v    print the lines that do not match
";

#[koru::main]
async fn main(args: Args) -> i32 {
    if args.size() == 1 || help_asked(&args) {
        return usage_asked(USAGE).await;
    }

    let mut invert = false;
    let mut fold = false;
    let mut i = 1;
    while i < args.size() {
        match &args[i] as &str {
            "-v" => invert = true,
            "-i" => fold = true,
            _ => break,
        }
        i += 1;
    }
    if i >= args.size() {
        return usage_error(USAGE).await;
    }

    // ASCII folding, which is what Braam's `fold_case` does.
    let pattern = if fold {
        args[i].to_ascii_lowercase()
    } else {
        args[i].to_string()
    };

    let mut input = File::over(Input::new(args.skip(i + 1), koru::stdin(), "grep"));
    let out = File::stdout();
    let mut line = String::new();
    let mut matched = false;

    loop {
        match input.getline(&mut line, false).await {
            Err(e) => return if e.is(Kind::Cancelled) { 130 } else { 1 },
            Ok(false) => break,
            Ok(true) => {}
        }

        let hay = if fold {
            line.to_ascii_lowercase()
        } else {
            line.to_string()
        };
        if hay.contains(&pattern) == invert {
            continue;
        }

        matched = true;
        line.push('\n');
        if out.write(&line).await.is_err() {
            return 1;
        }
    }

    if out.flush().await.is_err() {
        return 1;
    }
    i32::from(!matched)
}
