// SPDX-License-Identifier: MIT
//
// The `SYS_*` names a Braam program writes, over koru's own. Braam's kernel is
// its build, so these are constants in `kernel/sysabi.h`; here each is an alias
// for the libkoru name that already carries the value.
//
// **The three standard descriptors are the one place a value had to be taken
// rather than aliased.** Braam's `SYS_STDIN`, `SYS_STDOUT` and `SYS_STDERR`
// are 0, 1 and 2, and a program passes them as constants — `write_all(
// SYS_STDOUT, s)` — while koru's handles are `(index, generation)` pairs
// adopted at startup. So 0, 1 and 2 stay the constants they are, and every
// call that takes a `Handle` maps them through [`std_fd`]. A real koru handle
// has a generation of at least 1 in its high half and so is never below
// 1 << 16, which is what makes the mapping unambiguous and idempotent.

#ifndef KORU_SYSABI_HPP
#define KORU_SYSABI_HPP

#include <koru/ops.hpp>
#include <koru/vocab.hpp>

namespace koru {

inline constexpr Handle SYS_STDIN  = 0;
inline constexpr Handle SYS_STDOUT = 1;
inline constexpr Handle SYS_STDERR = 2;

inline constexpr u32 SYS_O_READ   = O_READ;
inline constexpr u32 SYS_O_WRITE  = O_WRITE;
inline constexpr u32 SYS_O_CREATE = O_CREATE;
inline constexpr u32 SYS_O_TRUNC  = O_TRUNC;
inline constexpr u32 SYS_O_APPEND = O_APPEND;
inline constexpr u32 SYS_O_EXCL   = O_EXCL;
inline constexpr u32 SYS_O_ALL    = O_ALL;

inline constexpr u32 SYS_SEEK_SET = SEEK_SET;
inline constexpr u32 SYS_SEEK_CUR = SEEK_CUR;
inline constexpr u32 SYS_SEEK_END = SEEK_END;
inline constexpr u64 SYS_SEEK_MAX = SEEK_MAX;

inline constexpr u32 SYS_CHUNK    = CHUNK;
inline constexpr u32 SYS_READ_MAX = READ_MAX;

// Braam's `kind` is a `u32`; koru's is an enumeration, so these are its
// enumerators and `e.kind == SYS_KIND_DIR` reads the same either way.
inline constexpr FileKind SYS_KIND_FILE = FileKind::File;
inline constexpr FileKind SYS_KIND_DIR  = FileKind::Dir;
inline constexpr FileKind SYS_KIND_LINK = FileKind::Link;

} // namespace koru

#endif // KORU_SYSABI_HPP
