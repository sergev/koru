// SPDX-License-Identifier: MIT

#include <koru/usage.hpp>

#include <koru/ops.hpp>
#include <koru/rt.hpp>

namespace koru {

task<i32> usage_asked(Str text)
{
    result<void> r = co_await write_all(out_fd(), text);
    co_return r.ok() ? 0 : 1;
}

task<i32> usage_error(Str text)
{
    co_await write_all(err_fd(), text);
    co_return 2;
}

} // namespace koru
