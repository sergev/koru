// SPDX-License-Identifier: MIT

//! Errnos, and the table mapping them onto Braam's fifteen names.
//!
//! The `Result` alias and the Braam type aliases are the `koru` crate's, which
//! re-exports [`Error`] rather than defining a second type. The `?`
//! conversions are here because the orphan rule puts them beside the type.

use std::fmt;
use std::io;

/// A positive errno. Negative values are never valid here; a `Cqe::res` is
/// converted with [`from_res`].
#[derive(Copy, Clone, PartialEq, Eq, Hash)]
pub struct Errno(pub i32);

impl Errno {
    /// The errno of the last failing libc call.
    pub fn last() -> Errno {
        Errno(io::Error::last_os_error().raw_os_error().unwrap_or(0))
    }

    /// The symbolic name, if this is one koru can produce.
    pub fn name(self) -> Option<&'static str> {
        KORU_ERRNOS.iter().find(|d| d.errno == self).map(|d| d.name)
    }

    pub fn as_io(self) -> io::Error {
        io::Error::from_raw_os_error(self.0)
    }
}

impl fmt::Debug for Errno {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self.name() {
            Some(n) => write!(f, "{n}({})", self.0),
            None => write!(f, "errno {}", self.0),
        }
    }
}

impl fmt::Display for Errno {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "{}", self.as_io())
    }
}

impl From<Errno> for io::Error {
    fn from(e: Errno) -> io::Error {
        e.as_io()
    }
}

impl TryFrom<&io::Error> for Errno {
    type Error = ();
    fn try_from(e: &io::Error) -> Result<Errno, ()> {
        e.raw_os_error().map(Errno).ok_or(())
    }
}

/// The `Cqe::res` sign convention, in exactly one place: `>= 0` is a result,
/// negative is `-errno`.
pub fn from_res(res: i64) -> Result<u64, Errno> {
    if res >= 0 {
        Ok(res as u64)
    } else {
        Err(Errno((-res) as i32))
    }
}

/// The inverse, for building expected values in tests.
pub fn to_res(r: Result<u64, Errno>) -> i64 {
    match r {
        Ok(v) => v as i64,
        Err(e) => -(e.0 as i64),
    }
}

// asm-generic errno values. See the architecture guard in `sys.rs`.
pub const EPERM: Errno = Errno(1);
pub const ENOENT: Errno = Errno(2);
pub const EINTR: Errno = Errno(4);
pub const EIO: Errno = Errno(5);
pub const ENXIO: Errno = Errno(6);
pub const EBADF: Errno = Errno(9);
pub const EAGAIN: Errno = Errno(11);
pub const ENOMEM: Errno = Errno(12);
pub const EACCES: Errno = Errno(13);
pub const EFAULT: Errno = Errno(14);
pub const EBUSY: Errno = Errno(16);
pub const EEXIST: Errno = Errno(17);
pub const EXDEV: Errno = Errno(18);
pub const ENOTDIR: Errno = Errno(20);
pub const EISDIR: Errno = Errno(21);
pub const EINVAL: Errno = Errno(22);
pub const EMFILE: Errno = Errno(24);
pub const ENOTTY: Errno = Errno(25);
pub const ENOSPC: Errno = Errno(28);
pub const EROFS: Errno = Errno(30);
pub const EPIPE: Errno = Errno(32);
pub const ERANGE: Errno = Errno(34);
pub const ENAMETOOLONG: Errno = Errno(36);
pub const ENOSYS: Errno = Errno(38);
pub const ENOTEMPTY: Errno = Errno(39);
pub const ELOOP: Errno = Errno(40);
pub const EPROTO: Errno = Errno(71);
pub const EOVERFLOW: Errno = Errno(75);
pub const EOPNOTSUPP: Errno = Errno(95);
pub const ETIMEDOUT: Errno = Errno(110);
pub const EALREADY: Errno = Errno(114);
pub const ECANCELED: Errno = Errno(125);

/// Braam's error vocabulary, with Braam's wire values.
///
/// [`Kind::Closed`] reaches a program two ways: `EPIPE`, and end of file,
/// which is `res == 0` and which T30's `read_chunk` turns into a name.
#[derive(Copy, Clone, PartialEq, Eq, Hash, Debug)]
#[repr(u8)]
pub enum Kind {
    Invalid = 1,
    NoMemory = 2,
    NotFound = 3,
    Exists = 4,
    NotDir = 5,
    IsDir = 6,
    Perm = 7,
    Io = 8,
    Cancelled = 9,
    Again = 10,
    Unsupported = 11,
    Closed = 12,
    NotEmpty = 13,
    Loop = 14,
    Intr = 15,
}

/// The fifteen, in wire order. `abi_dump` walks this rather than its own list.
pub const KINDS: [Kind; 15] = [
    Kind::Invalid,
    Kind::NoMemory,
    Kind::NotFound,
    Kind::Exists,
    Kind::NotDir,
    Kind::IsDir,
    Kind::Perm,
    Kind::Io,
    Kind::Cancelled,
    Kind::Again,
    Kind::Unsupported,
    Kind::Closed,
    Kind::NotEmpty,
    Kind::Loop,
    Kind::Intr,
];

impl Kind {
    /// Braam's `error_name`, word for word. Prose, so not in the ABI dump.
    pub fn name(self) -> &'static str {
        match self {
            Kind::Invalid => "invalid",
            Kind::NoMemory => "out of memory",
            Kind::NotFound => "not found",
            Kind::Exists => "already exists",
            Kind::NotDir => "not a directory",
            Kind::IsDir => "is a directory",
            Kind::Perm => "permission denied",
            Kind::Io => "i/o error",
            Kind::Cancelled => "cancelled",
            Kind::Again => "try again",
            Kind::Unsupported => "unsupported",
            Kind::Closed => "closed",
            Kind::NotEmpty => "directory not empty",
            Kind::Loop => "too many symbolic links",
            Kind::Intr => "interrupted",
        }
    }
}

/// One row: an errno, its name, the vocabulary name, and where it comes from.
pub struct ErrnoDef {
    pub errno: Errno,
    pub name: &'static str,
    pub kind: Kind,
    pub produced_by: &'static str,
}

/// Every errno koru can produce. A closed set: one outside it is a finding.
/// EBUSY, EALREADY, EMFILE, EPROTO and ENOTTY are judgement calls; see
/// doc/Notes.md.
pub const KORU_ERRNOS: &[ErrnoDef] = &[
    ErrnoDef {
        errno: EPERM,
        name: "EPERM",
        kind: Kind::Perm,
        produced_by: "filp_open, propagated verbatim",
    },
    ErrnoDef {
        errno: ENOENT,
        name: "ENOENT",
        kind: Kind::NotFound,
        produced_by: "OPEN of a missing path; CANCEL with no such op in flight",
    },
    ErrnoDef {
        errno: EINTR,
        name: "EINTR",
        kind: Kind::Intr,
        produced_by: "a signal during the ENTER wait; the writeback still happens",
    },
    ErrnoDef {
        errno: EIO,
        name: "EIO",
        kind: Kind::Io,
        produced_by: "kernel_read, propagated verbatim",
    },
    ErrnoDef {
        errno: ENXIO,
        name: "ENXIO",
        kind: Kind::NotFound,
        produced_by: "a write-only non-blocking OPEN of a FIFO with no reader",
    },
    ErrnoDef {
        errno: EBADF,
        name: "EBADF",
        kind: Kind::Invalid,
        produced_by: "a handle that is zero, stale, out of range or already closed",
    },
    ErrnoDef {
        errno: EAGAIN,
        name: "EAGAIN",
        kind: Kind::Again,
        produced_by: "a non-blocking READ or WRITE with nothing ready, or a failed enqueue",
    },
    ErrnoDef {
        errno: ENOMEM,
        name: "ENOMEM",
        kind: Kind::NoMemory,
        produced_by: "SETUP allocation, the OpWork allocation, or the OPEN path copy",
    },
    ErrnoDef {
        errno: EACCES,
        name: "EACCES",
        kind: Kind::Perm,
        produced_by: "OPEN permission check, in the submitting task's credentials",
    },
    ErrnoDef {
        errno: EFAULT,
        name: "EFAULT",
        kind: Kind::Invalid,
        produced_by: "an unreadable ioctl argument, or an SQE or CQE array that faults",
    },
    ErrnoDef {
        errno: EBUSY,
        name: "EBUSY",
        kind: Kind::Again,
        produced_by: "a second op on a busy slot; a second SETUP; a second mmap",
    },
    ErrnoDef {
        errno: EEXIST,
        name: "EEXIST",
        kind: Kind::Exists,
        produced_by: "filp_open, propagated verbatim",
    },
    ErrnoDef {
        errno: EXDEV,
        name: "EXDEV",
        kind: Kind::Unsupported,
        produced_by: "RENAME whose two paths are on different mounts",
    },
    ErrnoDef {
        errno: ENOTDIR,
        name: "ENOTDIR",
        kind: Kind::NotDir,
        produced_by: "OPEN with KORU_O_DIRECTORY on a non-directory",
    },
    ErrnoDef {
        errno: EISDIR,
        name: "EISDIR",
        kind: Kind::IsDir,
        produced_by: "OPEN of a directory for writing",
    },
    ErrnoDef {
        errno: EINVAL,
        name: "EINVAL",
        kind: Kind::Invalid,
        produced_by: "rule E1: a malformed SQE, unknown opcode, or a \
                      reserved field or flag bit set",
    },
    ErrnoDef {
        errno: EMFILE,
        name: "EMFILE",
        kind: Kind::NoMemory,
        produced_by: "the handle table is full",
    },
    ErrnoDef {
        errno: ENOTTY,
        name: "ENOTTY",
        kind: Kind::Unsupported,
        produced_by: "ioctl dispatch: foreign type byte or unknown command number",
    },
    ErrnoDef {
        errno: ENOSPC,
        name: "ENOSPC",
        kind: Kind::Io,
        produced_by: "the filesystem, propagated verbatim",
    },
    ErrnoDef {
        errno: EROFS,
        name: "EROFS",
        kind: Kind::Perm,
        produced_by: "filp_open on a read-only mount",
    },
    ErrnoDef {
        errno: EPIPE,
        name: "EPIPE",
        kind: Kind::Closed,
        produced_by: "WRITE to a pipe or socket whose reader is gone",
    },
    ErrnoDef {
        errno: ERANGE,
        name: "ERANGE",
        kind: Kind::Invalid,
        produced_by: "the filesystem, propagated verbatim",
    },
    ErrnoDef {
        errno: ENAMETOOLONG,
        name: "ENAMETOOLONG",
        kind: Kind::Invalid,
        produced_by: "a path component past NAME_MAX",
    },
    ErrnoDef {
        errno: ENOSYS,
        name: "ENOSYS",
        kind: Kind::Unsupported,
        produced_by: "the filesystem, propagated verbatim",
    },
    ErrnoDef {
        errno: ENOTEMPTY,
        name: "ENOTEMPTY",
        kind: Kind::NotEmpty,
        produced_by: "not reachable yet; RMDIR arrives at T27",
    },
    ErrnoDef {
        errno: ELOOP,
        name: "ELOOP",
        kind: Kind::Loop,
        produced_by: "KORU_O_NOFOLLOW on a symlink, or too many links",
    },
    ErrnoDef {
        errno: EPROTO,
        name: "EPROTO",
        kind: Kind::Unsupported,
        produced_by: "ioctl dispatch: right nr, wrong size or direction; \
                      or a bad SETUP magic or abi_version",
    },
    ErrnoDef {
        errno: EOVERFLOW,
        name: "EOVERFLOW",
        kind: Kind::Invalid,
        produced_by: "the filesystem, propagated verbatim",
    },
    ErrnoDef {
        errno: EOPNOTSUPP,
        name: "EOPNOTSUPP",
        kind: Kind::Unsupported,
        produced_by: "the filesystem, propagated verbatim",
    },
    ErrnoDef {
        errno: ETIMEDOUT,
        name: "ETIMEDOUT",
        kind: Kind::Io,
        produced_by: "the filesystem, propagated verbatim",
    },
    ErrnoDef {
        errno: EALREADY,
        name: "EALREADY",
        kind: Kind::Again,
        produced_by: "CANCEL found its target already running",
    },
    ErrnoDef {
        errno: ECANCELED,
        name: "ECANCELED",
        kind: Kind::Cancelled,
        produced_by: "the completion a successfully cancelled op receives",
    },
];

/// Map an errno onto the vocabulary. One outside [`KORU_ERRNOS`] falls back to
/// [`Kind::Io`] rather than panicking; the raw value is kept either way.
pub fn kind_of(errno: Errno) -> Kind {
    match KORU_ERRNOS.iter().find(|d| d.errno == errno) {
        Some(d) => d.kind,
        None => Kind::Io,
    }
}

/// A vocabulary name together with the raw errno it was built from.
#[derive(Copy, Clone, PartialEq, Eq)]
pub struct Error {
    kind: Kind,
    raw: Errno,
}

impl Error {
    pub fn from_errno(raw: Errno) -> Error {
        Error {
            kind: kind_of(raw),
            raw,
        }
    }

    /// End of file, which has no errno at all; raw 0 means synthesised here.
    /// The only such constructor, on purpose. doc/Notes.md says why.
    pub fn closed() -> Error {
        Error {
            kind: Kind::Closed,
            raw: Errno(0),
        }
    }

    pub fn kind(self) -> Kind {
        self.kind
    }

    /// The errno this was built from; preserved so the mapping is lossless.
    pub fn raw(self) -> Errno {
        self.raw
    }

    /// Braam's `e == Error::Cancelled`. `==` between two `Error`s compares
    /// the raw errno too, which keeps EBUSY and EALREADY distinct.
    pub fn is(self, kind: Kind) -> bool {
        self.kind == kind
    }
}

impl From<Errno> for Error {
    fn from(e: Errno) -> Error {
        Error::from_errno(e)
    }
}

/// So `err == Kind::Cancelled` reads as it does in Braam.
impl PartialEq<Kind> for Error {
    fn eq(&self, other: &Kind) -> bool {
        self.kind == *other
    }
}

impl PartialEq<Error> for Kind {
    fn eq(&self, other: &Error) -> bool {
        *self == other.kind
    }
}

/// `?` from any ordinary POSIX call. One carrying no errno becomes `Io` with
/// raw 0; every real errno round-trips.
impl From<io::Error> for Error {
    fn from(e: io::Error) -> Error {
        match e.raw_os_error() {
            Some(n) if n > 0 => Error::from_errno(Errno(n)),
            _ => Error {
                kind: Kind::Io,
                raw: Errno(0),
            },
        }
    }
}

impl From<Error> for io::Error {
    fn from(e: Error) -> io::Error {
        if e.raw.0 == 0 {
            // Only Closed gets here.
            io::Error::new(io::ErrorKind::UnexpectedEof, e)
        } else {
            e.raw.as_io()
        }
    }
}

impl fmt::Debug for Error {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "{:?}({:?})", self.kind, self.raw)
    }
}

impl fmt::Display for Error {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        // The OS message where there is one; it says more than the name.
        if self.raw.0 == 0 {
            write!(f, "{}", self.kind.name())
        } else {
            write!(f, "{}", self.raw)
        }
    }
}

impl std::error::Error for Error {}

#[cfg(test)]
mod tests {
    use super::*;
    use std::collections::HashSet;

    #[test]
    fn the_table_has_no_duplicate_errnos_or_names() {
        let mut errnos = HashSet::new();
        let mut names = HashSet::new();
        for d in KORU_ERRNOS {
            assert!(errnos.insert(d.errno), "duplicate errno {:?}", d.errno);
            assert!(names.insert(d.name), "duplicate name {}", d.name);
        }
    }

    /// Every row must be reachable through the lookup, not its fallback.
    #[test]
    fn every_errno_maps_to_exactly_one_kind_and_back_to_its_raw_value() {
        for d in KORU_ERRNOS {
            let e = Error::from_errno(d.errno);
            assert_eq!(e.kind(), d.kind, "{} mapped to the wrong name", d.name);
            assert_eq!(e.raw(), d.errno, "{} lost its raw errno", d.name);
            assert_eq!(d.errno.name(), Some(d.name));
        }
    }

    #[test]
    fn every_row_explains_where_it_comes_from() {
        for d in KORU_ERRNOS {
            assert!(!d.produced_by.is_empty(), "{} has no reason", d.name);
        }
    }

    /// Several errnos share a name on purpose; the raw value keeps it lossless.
    #[test]
    fn names_are_shared_but_raw_values_are_not() {
        assert_eq!(Error::from_errno(EBUSY).kind(), Kind::Again);
        assert_eq!(Error::from_errno(EALREADY).kind(), Kind::Again);
        assert_ne!(
            Error::from_errno(EBUSY).raw(),
            Error::from_errno(EALREADY).raw()
        );
    }

    #[test]
    fn res_sign_convention_round_trips() {
        assert_eq!(from_res(0), Ok(0));
        assert_eq!(from_res(4096), Ok(4096));
        assert_eq!(from_res(-22), Err(EINVAL));
        assert_eq!(to_res(Err(EINVAL)), -22);
        assert_eq!(to_res(Ok(4096)), 4096);
        // CHECKSUM masks to 63 bits, so the whole positive range must survive.
        assert_eq!(from_res(i64::MAX), Ok(i64::MAX as u64));
    }

    #[test]
    fn an_unknown_errno_falls_back_rather_than_panicking() {
        let odd = Errno(4095);
        assert_eq!(odd.name(), None);
        assert_eq!(Error::from_errno(odd).kind(), Kind::Io);
        assert_eq!(Error::from_errno(odd).raw(), odd);
    }

    #[test]
    fn the_fifteen_names_are_braams_own_wording_and_are_distinct() {
        assert_eq!(KINDS.len(), 15);
        let mut words = HashSet::new();
        for k in KINDS {
            assert!(words.insert(k.name()), "{k:?} shares a phrase");
            assert!(!k.name().is_empty());
        }
        // Spot-checked against Braam's error_name, which is the source.
        assert_eq!(Kind::NotFound.name(), "not found");
        assert_eq!(Kind::NoMemory.name(), "out of memory");
        assert_eq!(Kind::Loop.name(), "too many symbolic links");
        assert_eq!(Kind::Intr.name(), "interrupted");
    }

    #[test]
    fn closed_is_the_one_name_synthesised_rather_than_mapped() {
        let e = Error::closed();
        assert_eq!(e.kind(), Kind::Closed);
        assert_eq!(e.raw(), Errno(0), "no errno preimage");
        assert_eq!(e.to_string(), "closed");
        assert!(e.is(Kind::Closed));
    }

    #[test]
    fn an_error_compares_against_a_bare_name() {
        let e = Error::from_errno(ECANCELED);
        assert_eq!(e, Kind::Cancelled);
        assert_eq!(Kind::Cancelled, e);
        assert_ne!(e, Kind::Intr);
        // Two errnos sharing a name are still two errors.
        assert_ne!(Error::from_errno(EBUSY), Error::from_errno(EALREADY));
    }

    #[test]
    fn io_error_converts_both_ways() {
        let e = Error::from_errno(ENOENT);
        let io: io::Error = e.into();
        assert_eq!(io.raw_os_error(), Some(ENOENT.0));
        assert_eq!(Error::from(io), e);
        // No errno at all: Io, and honest about having no raw value.
        let other = Error::from(io::Error::other("not a syscall"));
        assert_eq!(other.kind(), Kind::Io);
        assert_eq!(other.raw(), Errno(0));
    }

    #[test]
    fn the_vocabulary_keeps_braams_wire_values() {
        assert_eq!(Kind::Invalid as u8, 1);
        assert_eq!(Kind::Io as u8, 8);
        assert_eq!(Kind::Closed as u8, 12);
        assert_eq!(Kind::Intr as u8, 15);
    }
}
