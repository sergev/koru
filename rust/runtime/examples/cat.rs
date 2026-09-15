// SPDX-License-Identifier: MIT

//! Braam's `src/cmd/cat.cpp`, in Rust. Chunks, not lines: `cat` is byte-exact,
//! so a last line without a newline stays that way.

use koru::{Args, File, Input, Kind, help_asked, usage_asked};

const USAGE: &str = "Usage:
    cat [<file>...]
";

#[koru::main]
async fn main(args: Args) -> i32 {
    if help_asked(&args) {
        return usage_asked(USAGE).await;
    }

    let mut input = File::over(Input::new(args.tail(), koru::stdin(), "cat"));
    let out = File::stdout();

    let mut buf = [0u8; 256];
    loop {
        let n = match input.read(&mut buf).await {
            Ok(n) => n,
            Err(_) => break, // the error sticks on the stream and is read below
        };
        if out.write_bytes(&buf[..n]).await.is_err() {
            break;
        }
    }

    if out.flush().await.is_err() {
        return 1;
    }
    if input.failed() {
        return match input.err() {
            Some(e) if e.is(Kind::Cancelled) => 130,
            _ => 1,
        };
    }
    0
}
