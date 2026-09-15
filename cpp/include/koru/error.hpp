// SPDX-License-Identifier: MIT
//
// Errnos, and the table mapping them onto Braam's fifteen names. The C++ half
// of what rust/sys/src/error.rs is on the Rust side.
//
// The table is not written twice: both enumerations come from the X-macros in
// cpp/include/koru_errno.h, which is the mirror scripts/abi.sh diffs. The
// numbers there are asm-generic, and a static_assert per row checks them
// against this host's <cerrno> — which is a guard the Rust side cannot have,
// because it has no libc headers to disagree with.
//
// There are no errno constants in this namespace on purpose: EINVAL and its
// kin are macros, and `koru::EINVAL` would be `koru::22`. Spell the libc macro
// inside an Errno instead.

#ifndef KORU_ERROR_HPP
#define KORU_ERROR_HPP

#include <koru/result.hpp>
#include <koru_errno.h>

#include <cerrno>
#include <cstddef>
#include <cstdint>

namespace koru {

/// A positive errno. A `koru_cqe::res` is negative and is converted by
/// [`from_res`].
struct Errno {
    int value = 0;

    constexpr Errno() = default;
    constexpr explicit Errno(int v) : value(v) {}

    friend constexpr bool operator==(Errno a, Errno b) { return a.value == b.value; }
    friend constexpr bool operator!=(Errno a, Errno b) { return a.value != b.value; }

    /// The symbolic name, or nullptr where this is not one koru can produce.
    const char *name() const;

    /// strerror, which says more than the name.
    const char *message() const;

    /// The errno of the last failing libc call.
    static Errno last() { return Errno(errno); }
};

/// Braam's error vocabulary, with Braam's wire values.
enum class Kind : uint8_t {
#define KORU_KIND_CASE(name, value) name = (value),
    KORU_KIND_TABLE(KORU_KIND_CASE)
#undef KORU_KIND_CASE
};

/// The fifteen, in wire order.
extern const Kind KINDS[15];

/// Braam's `error_name`, word for word.
const char *kind_name(Kind k);

/// One row of the table: an errno, its symbolic name, and its vocabulary name.
/// The provenance prose stays in error.rs, as koru_errno.h says.
struct ErrnoDef {
    Errno err;
    const char *name;
    Kind kind;
};

extern const ErrnoDef KORU_ERRNOS[];
size_t koru_errnos_len();

/// The vocabulary name for an errno. Anything outside the table is `Io`, which
/// is a fallback, not a row.
Kind kind_of(Errno e);

/// A vocabulary name together with the raw errno it was built from.
class Error {
public:
    Error() = default;

    /// A vocabulary name with no errno behind it. Braam's `Error` **is** the
    /// vocabulary, so this conversion is what lets a program write it.
    Error(Kind k) : kind_(k) {}

    static Error from_errno(Errno raw) { return Error(kind_of(raw), raw); }

    /// End of file, which has no errno at all; raw 0 means synthesised here.
    /// The only such constructor, on purpose. doc/Notes.md says why.
    static Error closed() { return Error(Kind::Closed, Errno(0)); }

    /// A vocabulary name with no errno behind it: `Err(Error::NoMemory)` names
    /// a failure the kernel was never asked about. doc/Notes.md says why this
    /// does not weaken `closed()`'s rule.
    static Error of(Kind k) { return Error(k, Errno(0)); }

    /// Braam's enumerators, as names in the type that carries them. Each is a
    /// [`Kind`], not a second enumeration.
#define KORU_KIND_NAME(name, value) static constexpr Kind name = Kind::name;
    KORU_KIND_TABLE(KORU_KIND_NAME)
#undef KORU_KIND_NAME

    Kind kind() const { return kind_; }
    Errno raw() const { return raw_; }

    /// Braam's `e == Error::Cancelled`.
    bool is(Kind k) const { return kind_ == k; }

    /// The OS message where there is one; the vocabulary name otherwise.
    const char *message() const;

    friend bool operator==(Error a, Error b) { return a.kind_ == b.kind_ && a.raw_ == b.raw_; }
    friend bool operator!=(Error a, Error b) { return !(a == b); }

    friend bool operator==(Error a, Kind k) { return a.kind_ == k; }
    friend bool operator==(Kind k, Error a) { return a.kind_ == k; }
    friend bool operator!=(Error a, Kind k) { return a.kind_ != k; }
    friend bool operator!=(Kind k, Error a) { return a.kind_ != k; }

private:
    Error(Kind k, Errno raw) : kind_(k), raw_(raw) {}

    Kind kind_ = Kind::Io;
    Errno raw_ = Errno(0);
};

/// Braam's `Err(...)`, which tags a value as the failing side of a `Result`.
/// Here the tag is not needed — `result<T, E>` tells the two apart by type —
/// so both forms simply build the `Error` the result is constructed from.
inline Error Err(Error e)
{
    return e;
}

inline Error Err(Kind k)
{
    return Error::of(k);
}

inline Error Err(Errno e)
{
    return Error::from_errno(e);
}

/// The `res` sign convention, in exactly one place: `>= 0` is a result,
/// negative is `-errno`.
result<uint64_t, Errno> from_res(int64_t res);

/// The inverse, for building expected values in tests.
int64_t to_res(result<uint64_t, Errno> r);

// The mirror's numbers are asm-generic. On a host whose libc disagrees this
// binding would be silently wrong, so say so at compile time.
#define KORU_ERRNO_AGREES(name, value, kind) \
    static_assert(name == (value), #name " is not its asm-generic value on this host");
KORU_ERRNO_TABLE(KORU_ERRNO_AGREES)
#undef KORU_ERRNO_AGREES

} // namespace koru

#endif // KORU_ERROR_HPP
