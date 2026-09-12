// SPDX-License-Identifier: GPL-2.0

//! Errnos, and the table mapping them onto Braam's fifteen names.
//!
//! T20 adds the `Result` alias, the `?` conversions and the Braam aliases in
//! the `koru` crate, re-exporting [`Error`] rather than defining a second type.

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
pub const EBADF: Errno = Errno(9);
pub const EAGAIN: Errno = Errno(11);
pub const ENOMEM: Errno = Errno(12);
pub const EACCES: Errno = Errno(13);
pub const EFAULT: Errno = Errno(14);
pub const EBUSY: Errno = Errno(16);
pub const EEXIST: Errno = Errno(17);
pub const ENOTDIR: Errno = Errno(20);
pub const EISDIR: Errno = Errno(21);
pub const EINVAL: Errno = Errno(22);
pub const EMFILE: Errno = Errno(24);
pub const ENOTTY: Errno = Errno(25);
pub const ENOSPC: Errno = Errno(28);
pub const EROFS: Errno = Errno(30);
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
/// [`Kind::Closed`] has no errno preimage: EOF is `res == 0`, and T30's
/// `read_chunk` is what turns one into the other.
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
        errno: EBADF,
        name: "EBADF",
        kind: Kind::Invalid,
        produced_by: "a handle that is zero, stale, out of range or already closed",
    },
    ErrnoDef {
        errno: EAGAIN,
        name: "EAGAIN",
        kind: Kind::Again,
        produced_by: "enqueue_delayed failed to queue a deferred op",
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

    pub fn kind(self) -> Kind {
        self.kind
    }

    /// The errno this was built from; preserved so the mapping is lossless.
    pub fn raw(self) -> Errno {
        self.raw
    }
}

impl From<Errno> for Error {
    fn from(e: Errno) -> Error {
        Error::from_errno(e)
    }
}

impl fmt::Debug for Error {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "{:?}({:?})", self.kind, self.raw)
    }
}

impl fmt::Display for Error {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "{}", self.raw)
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
    fn the_vocabulary_keeps_braams_wire_values() {
        assert_eq!(Kind::Invalid as u8, 1);
        assert_eq!(Kind::Io as u8, 8);
        assert_eq!(Kind::Closed as u8, 12);
        assert_eq!(Kind::Intr as u8, 15);
    }
}
