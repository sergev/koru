// SPDX-License-Identifier: MIT

//! Braam's `src/cmd/less.cpp`, in Rust: a pager on a terminal, a `cat` when
//! stdout is not one.
//!
//! This is Phase 9's deliverable, and the point of it is what it does *not*
//! do: it names no socket, no daemon, no protocol and no window. It claims the
//! keys, claims the screen, paints panes and reads keys — the same six methods
//! Braam's pager calls, over a transport that did not exist when it was
//! written.
//!
//! It reads its input to the end before it paints, which is not the laziness a
//! real `less` has; Braam's has the same limit for its own reason.

use koru::ks_abi::{KS_COLOR_BLACK, KS_COLOR_CYAN};
use koru::{
    Args, Grid, Input, KEY_DOWN, KEY_END, KEY_ENTER, KEY_ESCAPE, KEY_HOME, KEY_PAGE_DOWN,
    KEY_PAGE_UP, KEY_UP, Key, Kind, LineReader, Screen, TextBuf, TextView,
};

const USAGE: &str = "Usage:
    less [<file>]
";

struct Pager {
    buf: TextBuf,
    view: TextView,
    name: String,
}

fn paint(p: &mut Pager, screen: &mut Screen) {
    let mut body = screen.body();
    let h = body.height();
    {
        let grid: &mut Grid = screen.grid();
        p.view.paint(&mut body, grid, &p.buf);
    }

    let lines = p.buf.lines();
    let shown = lines.min(p.view.top() + h as usize);
    let pct = if lines > 0 { shown * 100 / lines } else { 100 };
    let name = if p.name.is_empty() { "stdin" } else { &p.name };
    let line = format!(
        " {name}  {pct}%  {}— q quits, arrows and PgUp/PgDn scroll",
        if shown == lines { "END  " } else { "" }
    );

    let mut bar = screen.status();
    bar.style(KS_COLOR_BLACK, KS_COLOR_CYAN, 0);
    bar.move_to(0, 0);
    let grid: &mut Grid = screen.grid();
    bar.write(grid, &line);
    bar.fill_row(grid);
}

#[koru::main]
async fn main(args: Args) -> i32 {
    if koru::help_asked(&args) {
        return koru::usage_asked(USAGE).await;
    }
    if args.size() > 2 {
        return koru::usage_error(USAGE).await;
    }

    // No terminal is not an error: it is `cat`. The screen connection is the
    // whole of the question — with no daemon and none to start, there is
    // nothing to paint on.
    let mut screen = match Screen::connect().await {
        Ok(s) => s,
        Err(_) => return cat(&args).await,
    };

    // The keys before the input is read, so that anything typed while a slow
    // pipe fills is queued for us rather than echoed at the shell.
    if screen.take_keys().await.is_err() {
        let _ = koru::write_all(koru::stderr(), "less: no keyboard\n").await;
        return 1;
    }

    let mut p = Pager {
        buf: TextBuf::default(),
        view: TextView::default(),
        name: if args.size() == 2 {
            args[1].to_string()
        } else {
            String::new()
        },
    };

    let mut lines = LineReader::new(Input::new(args.tail(), koru::stdin(), "less"));
    let mut line = String::new();
    loop {
        match lines.next(&mut line).await {
            Ok(true) => p.buf.add(&line),
            Ok(false) => break,
            Err(e) if e.is(Kind::Cancelled) => return 130,
            Err(_) => return 1,
        }
    }

    if screen.take_screen().await.is_err() {
        let _ = koru::write_all(koru::stderr(), "less: no screen\n").await;
        return 1;
    }

    loop {
        paint(&mut p, &mut screen);
        if screen.flush().await.is_err() {
            return 1;
        }

        let k: Key = match screen.next_key().await {
            Ok(k) => k,
            // Intr is a resize with no key behind it: the grid is already the
            // new shape, so the top of the loop repaints it.
            Err(e) if e.is(Kind::Intr) || e.is(Kind::Again) => continue,
            Err(e) if e.is(Kind::Cancelled) => return 130,
            Err(_) => return 1,
        };

        let h = screen.body().height();
        let page = if h > 1 { h - 1 } else { 1 };
        let lines = p.buf.lines();

        match k.code {
            c if c == u32::from('q') || c == u32::from('Q') || c == KEY_ESCAPE => return 0,
            c if c == KEY_UP || c == u32::from('k') => p.view.scroll(-1, lines, h),
            c if c == KEY_DOWN || c == u32::from('j') || c == KEY_ENTER => {
                p.view.scroll(1, lines, h)
            }
            c if c == KEY_PAGE_UP || c == u32::from('b') => p.view.scroll(-(page as i32), lines, h),
            c if c == KEY_PAGE_DOWN || c == u32::from(' ') || c == u32::from('f') => {
                p.view.scroll(page as i32, lines, h)
            }
            c if c == KEY_HOME || c == u32::from('g') => p.view.to(0, lines, h),
            c if c == KEY_END || c == u32::from('G') => p.view.to(lines, lines, h),
            _ => {}
        }
    }
}

/// `cat.cpp`'s loop: chunks, so a last line without a newline stays one.
async fn cat(args: &Args) -> i32 {
    let mut files = Input::new(args.tail(), koru::stdin(), "less");
    loop {
        match files.read().await {
            Ok(chunk) => {
                // Bytes, not a string: a chunk boundary may fall inside a
                // UTF-8 sequence, which is T31's finding and not this file's.
                if koru::write_all(koru::stdout(), &String::from_utf8_lossy(&chunk))
                    .await
                    .is_err()
                {
                    return 1;
                }
            }
            Err(e) if e.is(Kind::Closed) => return 0,
            Err(e) if e.is(Kind::Cancelled) => return 130,
            Err(_) => return 1,
        }
    }
}
