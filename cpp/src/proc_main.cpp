// SPDX-License-Identifier: MIT
//
// Braam's program entry. A `src/cmd` source defines `proc_main` and returns a
// bare status; libkoru's own entry is `koru_main` and returns a `Result<i32>`,
// because `CO_TRY` must have somewhere to send an error. This adapts the one
// to the other, and is a library of its own — `koru_start_proc` — so a binary
// links whichever entry it writes and never owes both.

#include <koru/braam.hpp>

koru::task<koru::result<koru::i32>> koru_main(koru::Args args)
{
    co_return co_await proc_main(std::move(args));
}
