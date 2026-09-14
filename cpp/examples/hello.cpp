// SPDX-License-Identifier: MIT
//
// T44's done test: Braam's `examples/hello/hello.cpp`, unchanged but for the
// include line. It names no executor, no ring, no namespace and no header but
// this one, and `rust/runtime/examples/hello.rs` is the same program in the
// other binding — scripts/cpp.sh runs both and compares the bytes.

#include <koru/braam.hpp>

Task<Result<i32>> koru_main(Args args)
{
    Str who = "world";
    if (args.size() > 1)
        who = args[1];

    CO_TRY_VOID(co_await write_all(out_fd(), "Hello, "));
    CO_TRY_VOID(co_await write_all(out_fd(), who));
    CO_TRY_VOID(co_await write_all(out_fd(), "!\n"));

    co_return 0;
}
