// SPDX-License-Identifier: MIT
//
// Braam's `proc/usage.h`: a usage block, asked for or got wrong. The C++ half
// of rust/runtime/src/usage.rs.
//
// Raw `write_all`, not the buffered `File`, as Braam's is and as `errln` is:
// the block is one write and nothing follows it.

#ifndef KORU_USAGE_HPP
#define KORU_USAGE_HPP

#include <koru/task.hpp>
#include <koru/vocab.hpp>

namespace koru {

/// The block on stdout, and 0.
task<i32> usage_asked(Str text);

/// The block on stderr, and 2.
task<i32> usage_error(Str text);

} // namespace koru

#endif // KORU_USAGE_HPP
