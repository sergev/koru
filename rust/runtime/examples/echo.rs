// SPDX-License-Identifier: MIT

//! Braam's `src/cmd/echo.cpp`, in Rust — the same program against the same
//! function names, written the way Rust would write it. `cpp/cmd/echo.cpp` is
//! Braam's own source compiled against koru, and scripts/portability.sh runs
//! the two and compares the bytes.

use koru::{Args, Buffering, File};

#[koru::main]
async fn main(args: Args) -> i32 {
    // Full rather than Auto: the whole output is one flush.
    File::stdout().set_buffering(Buffering::Full);

    let mut i = 1;
    let newline = !(i < args.size() && &args[i] == "-n");
    if !newline {
        i += 1;
    }

    for (n, w) in (i..args.size()).enumerate() {
        if n > 0 && File::stdout().write(" ").await.is_err() {
            return 1;
        }
        if File::stdout().write(&args[w]).await.is_err() {
            return 1;
        }
    }
    if newline && File::stdout().write("\n").await.is_err() {
        return 1;
    }

    i32::from(File::stdout().flush().await.is_err())
}
