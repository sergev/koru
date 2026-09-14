// SPDX-License-Identifier: MIT
//
// Braam's error vocabulary and the closed set of errnos koru can produce.
// Hand-written mirror of rust/sys/src/error.rs; scripts/abi.sh diffs the
// two through the abi_dump binaries.
//
// Each row's provenance prose lives in error.rs and is deliberately not
// mirrored here: it is documentation, not wire format, and diffing it would
// make a wording change a two-language edit.
//
// X-macro rather than an array, so nothing is defined in the header: a
// static const array in a header is -Wunused-variable in any C translation
// unit that does not use it.

#ifndef KORU_ERRNO_H
#define KORU_ERRNO_H

// The fifteen names, with Braam's wire values. Closed reaches userspace two
// ways: EPIPE from a write whose reader is gone, and end of file, which is
// res == 0 and which the read wrapper is what turns into a name.
#define KORU_KIND_TABLE(X) \
    X(Invalid, 1)          \
    X(NoMemory, 2)         \
    X(NotFound, 3)         \
    X(Exists, 4)           \
    X(NotDir, 5)           \
    X(IsDir, 6)            \
    X(Perm, 7)             \
    X(Io, 8)               \
    X(Cancelled, 9)        \
    X(Again, 10)           \
    X(Unsupported, 11)     \
    X(Closed, 12)          \
    X(NotEmpty, 13)        \
    X(Loop, 14)            \
    X(Intr, 15)

#define KORU_KIND_ENUMERATOR(name, value) KORU_KIND_##name = (value),

enum koru_kind { KORU_KIND_TABLE(KORU_KIND_ENUMERATOR) };

// Every errno koru can produce, ascending. A value outside this set is a
// finding. EBUSY, EALREADY, EMFILE, EPROTO and ENOTTY are judgement calls;
// see doc/Notes.md. asm-generic values: alpha, mips, parisc and sparc differ.
#define KORU_ERRNO_TABLE(X)        \
    X(EPERM, 1, Perm)              \
    X(ENOENT, 2, NotFound)         \
    X(EINTR, 4, Intr)              \
    X(EIO, 5, Io)                  \
    X(ENXIO, 6, NotFound)          \
    X(EBADF, 9, Invalid)           \
    X(EAGAIN, 11, Again)           \
    X(ENOMEM, 12, NoMemory)        \
    X(EACCES, 13, Perm)            \
    X(EFAULT, 14, Invalid)         \
    X(EBUSY, 16, Again)            \
    X(EEXIST, 17, Exists)          \
    X(EXDEV, 18, Unsupported)      \
    X(ENOTDIR, 20, NotDir)         \
    X(EISDIR, 21, IsDir)           \
    X(EINVAL, 22, Invalid)         \
    X(EMFILE, 24, NoMemory)        \
    X(ENOTTY, 25, Unsupported)     \
    X(ENOSPC, 28, Io)              \
    X(EROFS, 30, Perm)             \
    X(EPIPE, 32, Closed)           \
    X(ERANGE, 34, Invalid)         \
    X(ENAMETOOLONG, 36, Invalid)   \
    X(ENOSYS, 38, Unsupported)     \
    X(ENOTEMPTY, 39, NotEmpty)     \
    X(ELOOP, 40, Loop)             \
    X(EPROTO, 71, Unsupported)     \
    X(EOVERFLOW, 75, Invalid)      \
    X(EOPNOTSUPP, 95, Unsupported) \
    X(ECONNRESET, 104, Closed)     \
    X(ETIMEDOUT, 110, Io)          \
    X(EALREADY, 114, Again)        \
    X(ECANCELED, 125, Cancelled)

#endif // KORU_ERRNO_H
