// SPDX-License-Identifier: MIT
//
// Braam's `fs/path.h`: text, not paths. Nothing here opens anything, and
// nothing here reaches the ring — which is what lets `basename`, `dirname` and
// `ln` use it unchanged.
//
// Braam's rule is that a resolved path is absolute, '/'-separated, and never
// ends in a slash except for the root. koru's filesystem is the host's and
// resolution happens in the kernel, so `path_resolve` here is the same pure
// normaliser it is there and is not what `open_at` calls.

#ifndef KORU_PATH_HPP
#define KORU_PATH_HPP

#include <koru/vocab.hpp>

namespace koru {

/// Resolves `p` against `cwd` and normalises: '.' drops, '..' pops, repeated
/// and trailing slashes go. A '..' at the root stays at the root.
result<void> path_resolve(Str cwd, Str p, String &out);

/// Everything before the last '/', or "/" when there is nothing.
Str path_dirname(Str p);

/// Everything after the last '/'. The root's basename is "/".
Str path_basename(Str p);

/// Appends `name` to `dir` with exactly one separator between them.
result<void> path_join(Str dir, Str name, String &out);

/// Whether `p` is `prefix` or lies beneath it. "/" is a prefix of everything.
bool path_under(Str prefix, Str p);

} // namespace koru

#endif // KORU_PATH_HPP
