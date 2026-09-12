// SPDX-License-Identifier: MIT

//! Braam's `examples/hello/hello.cpp`, in Rust. Its C++ original is
//! seventeen lines and names no executor; so does this.

use koru::{Args, Result, write_all};

#[koru::main]
async fn main(args: Args) -> Result<i32> {
    let mut who = "world";
    if args.size() > 1 {
        who = &args[1];
    }

    write_all(koru::stdout(), "Hello, ").await?;
    write_all(koru::stdout(), who).await?;
    write_all(koru::stdout(), "!\n").await?;

    Ok(0)
}
