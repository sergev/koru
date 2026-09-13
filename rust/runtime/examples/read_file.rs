// SPDX-License-Identifier: MIT

//! T15's demo against the ambient ring: a file read through the ring while
//! timers complete out of order, printed with `WRITE` rather than `println!`.
//!
//! The bytes on stdout are the file and nothing else — T42's C++ demo has to
//! match them. The completion order goes to stderr.

use koru::{Args, Result, spawn, write_all};
use koru_sys::abi::KORU_O_RDONLY;
use std::cell::RefCell;
use std::rc::Rc;

const MS: u64 = 1_000_000;

#[koru::main]
async fn main(args: Args) -> Result<i32> {
    let path = if args.size() > 1 {
        &args[1]
    } else {
        "/etc/hosts"
    };
    let rt = koru::rt::current();
    let order = Rc::new(RefCell::new(Vec::new()));

    // Armed longest first, so submission order and completion order differ.
    for ms in [30u64, 10, 20] {
        let rt2 = rt.clone();
        let order2 = Rc::clone(&order);
        spawn(async move {
            let _ = rt2.delay(ms * MS).await;
            order2.borrow_mut().push(ms);
        });
    }

    let slot = rt.acquire().expect("a slot");
    let (h, slot) = rt.open(slot, path, KORU_O_RDONLY, 0).await;
    let h = h?;
    let len = slot.len() as u32;
    let (n, slot) = rt.read(h, slot, 0, len).await;
    let n = n?;
    let text = String::from_utf8_lossy(&slot[..n]).into_owned();
    drop(slot);
    koru::close_fd(h).await;

    write_all(koru::stdout(), &text).await?;

    // The timers ran while the file was being opened and read; this waits for
    // the stragglers without a nested block_on, which would re-enter the
    // executor the outer one is already inside.
    while order.borrow().len() < 3 {
        rt.delay(MS).await?;
    }

    let line = order
        .borrow()
        .iter()
        .map(u64::to_string)
        .collect::<Vec<_>>()
        .join(" ");
    write_all(koru::stderr(), &format!("{line}\n")).await?;
    Ok(0)
}
