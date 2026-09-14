// SPDX-License-Identifier: MIT

//! A koru program that uses the screen, for the lifecycle checks to drive.
//!
//! It is not a demo: `less` is the demo. This exists so a shell script can
//! start twenty of it at once, kill one mid-blit, or kill the daemon under it,
//! and see what a client does — none of which a program written for a person
//! would make easy.
//!
//!   screen_probe connect        connect and leave, claiming nothing
//!   screen_probe hold <ms>      keep the screen, painting, for that long
//!   screen_probe keys           take the keys and report the first one

use koru::{Args, Kind, Screen};

#[koru::main]
async fn main(args: Args) -> i32 {
    let mode = if args.size() > 1 { &args[1] } else { "connect" };

    let mut screen = match Screen::connect().await {
        Ok(s) => s,
        Err(e) => {
            koru::errln("screen_probe", "connect", e).await;
            return 1;
        }
    };

    if mode == "keys" {
        if let Err(e) = screen.take_keys().await {
            koru::errln("screen_probe", "keys", e).await;
            return 1;
        }
        eprintln!("ready");
        return match screen.next_key().await {
            Ok(k) => {
                eprintln!("key {} mods {}", k.code, k.mods);
                0
            }
            Err(e) => {
                koru::errln("screen_probe", "key", e).await;
                1
            }
        };
    }

    if mode == "connect" {
        // Claims nothing: the screen is exclusive, so twenty clients that all
        // took it would be nineteen refusals and no information about the
        // spawn at all.
        eprintln!("connected {}", std::process::id());
        return 0;
    }

    if let Err(e) = screen.take_screen().await {
        koru::errln("screen_probe", "screen", e).await;
        return 1;
    }

    let mut p = screen.root();
    let text = format!("probe {}", std::process::id());
    let grid = screen.grid();
    p.write_at(grid, 0, 0, &text);
    if let Err(e) = screen.flush().await {
        koru::errln("screen_probe", "flush", e).await;
        return 1;
    }
    eprintln!("connected {}", std::process::id());

    if mode == "hold" {
        let ms: u32 = if args.size() > 2 {
            args[2].parse().unwrap_or(1000)
        } else {
            1000
        };
        // Painting the whole time, so a daemon that dies is noticed here
        // rather than at exit — which is the difference between reporting a
        // dead connection and hanging on one.
        for i in 0..ms / 20 {
            let mut p = screen.root();
            let text = format!("hold {i}");
            let grid = screen.grid();
            p.write_at(grid, 0, 1, &text);
            if let Err(e) = screen.flush().await {
                let dead = e.is(Kind::Closed);
                koru::errln("screen_probe", "flush", e).await;
                return if dead { 2 } else { 1 };
            }
            if koru::sleep_for(20).await.is_err() {
                return 130;
            }
        }
    }
    0
}
