// SPDX-License-Identifier: MIT

//! The libc surface, declared by hand. Everything else comes from `std`.
//!
//! Nothing checks these prototypes; no assertion can. Re-read them against the
//! C headers on a libc bump.

use crate::abi::{KoruEnter, KoruParams};
use std::ffi::{c_int, c_uint, c_ulong, c_void};
use std::io;
use std::os::fd::{AsRawFd, BorrowedFd};

// The ioctl encoding and the errno numbers are asm-generic; alpha, mips, parisc
// and sparc differ in both.
#[cfg(not(all(
    target_os = "linux",
    any(
        target_arch = "x86_64",
        target_arch = "aarch64",
        target_arch = "riscv64"
    )
)))]
compile_error!("koru-sys hardcodes asm-generic ioctl encodings and errno values");

pub type PidT = i32;
pub type UidT = u32;
pub type GidT = u32;
pub type OffT = i64;
pub type ModeT = u32;

unsafe extern "C" {
    /// `int ioctl(int fd, unsigned long request, ...);`
    pub fn ioctl(fd: c_int, request: c_ulong, ...) -> c_int;

    /// `void *mmap(void *addr, size_t length, int prot, int flags, int fd, off_t offset);`
    pub fn mmap(
        addr: *mut c_void,
        length: usize,
        prot: c_int,
        flags: c_int,
        fd: c_int,
        offset: OffT,
    ) -> *mut c_void;

    /// `int munmap(void *addr, size_t length);`
    pub fn munmap(addr: *mut c_void, length: usize) -> c_int;

    /// `int madvise(void *addr, size_t length, int advice);`
    pub fn madvise(addr: *mut c_void, length: usize, advice: c_int) -> c_int;

    /// `pid_t fork(void);`
    pub fn fork() -> PidT;

    /// `pid_t waitpid(pid_t pid, int *wstatus, int options);`
    pub fn waitpid(pid: PidT, wstatus: *mut c_int, options: c_int) -> PidT;

    /// `int kill(pid_t pid, int sig);`
    pub fn kill(pid: PidT, sig: c_int) -> c_int;

    /// `sighandler_t signal(int signum, sighandler_t handler);`
    pub fn signal(signum: c_int, handler: usize) -> usize;

    /// `unsigned int alarm(unsigned int seconds);`
    pub fn alarm(seconds: c_uint) -> c_uint;

    /// `void _exit(int status);`
    pub fn _exit(status: c_int) -> !;

    /// `uid_t geteuid(void);`
    pub fn geteuid() -> UidT;

    /// `int setresuid(uid_t ruid, uid_t euid, uid_t suid);`
    pub fn setresuid(ruid: UidT, euid: UidT, suid: UidT) -> c_int;

    /// `int setresgid(gid_t rgid, gid_t egid, gid_t sgid);`
    pub fn setresgid(rgid: GidT, egid: GidT, sgid: GidT) -> c_int;

    /// `int setgroups(size_t size, const gid_t *list);`
    pub fn setgroups(size: usize, list: *const GidT) -> c_int;

    /// `int mkfifo(const char *pathname, mode_t mode);`
    pub fn mkfifo(pathname: *const std::ffi::c_char, mode: ModeT) -> c_int;

    /// `mode_t umask(mode_t mask);` — `MKDIR`'s mode passes through it.
    pub fn umask(mask: ModeT) -> ModeT;
}

pub const PROT_READ: c_int = 0x1;
pub const PROT_WRITE: c_int = 0x2;
pub const MAP_SHARED: c_int = 0x01;
pub const MAP_PRIVATE: c_int = 0x02;
pub const MAP_FAILED: *mut c_void = usize::MAX as *mut c_void;
pub const MADV_DOFORK: c_int = 11;

pub const SIGINT: c_int = 2;
pub const SIGKILL: c_int = 9;
pub const SIGALRM: c_int = 14;
pub const SIG_DFL: usize = 0;

/// `WIFEXITED(status)`
pub const fn wifexited(status: c_int) -> bool {
    (status & 0x7f) == 0
}

/// `WEXITSTATUS(status)`
pub const fn wexitstatus(status: c_int) -> c_int {
    (status >> 8) & 0xff
}

/// `WIFSIGNALED(status)`
pub const fn wifsignaled(status: c_int) -> bool {
    ((status & 0x7f) + 1) >> 1 > 0
}

/// `WTERMSIG(status)`
pub const fn wtermsig(status: c_int) -> c_int {
    status & 0x7f
}

// ---------------------------------------------------------------------------
// ioctl encoding
// ---------------------------------------------------------------------------
//
// Mirrors the kernel's `kernel::ioctl::_IOWR::<T>`: the numbers are derived
// from the struct sizes on both sides.

pub const _IOC_NRBITS: u32 = 8;
pub const _IOC_TYPEBITS: u32 = 8;
pub const _IOC_SIZEBITS: u32 = 14;

pub const _IOC_NRSHIFT: u32 = 0;
pub const _IOC_TYPESHIFT: u32 = _IOC_NRSHIFT + _IOC_NRBITS;
pub const _IOC_SIZESHIFT: u32 = _IOC_TYPESHIFT + _IOC_TYPEBITS;
pub const _IOC_DIRSHIFT: u32 = _IOC_SIZESHIFT + _IOC_SIZEBITS;

pub const _IOC_NONE: u32 = 0;
pub const _IOC_WRITE: u32 = 1;
pub const _IOC_READ: u32 = 2;

/// Assemble an ioctl request number. The tests build malformed requests with
/// it too, so there is one implementation, not two.
pub const fn ioc(dir: u32, ty: u32, nr: u32, size: usize) -> u32 {
    assert!(size < (1usize << _IOC_SIZEBITS));
    (dir << _IOC_DIRSHIFT)
        | (ty << _IOC_TYPESHIFT)
        | (nr << _IOC_NRSHIFT)
        | ((size as u32) << _IOC_SIZESHIFT)
}

pub const fn io_r<T>(ty: u32, nr: u32) -> u32 {
    ioc(_IOC_READ, ty, nr, size_of::<T>())
}

pub const fn io_w<T>(ty: u32, nr: u32) -> u32 {
    ioc(_IOC_WRITE, ty, nr, size_of::<T>())
}

pub const fn io_wr<T>(ty: u32, nr: u32) -> u32 {
    ioc(_IOC_READ | _IOC_WRITE, ty, nr, size_of::<T>())
}

use crate::abi::{KORU_IOC_TYPE, KORU_NR_ENTER, KORU_NR_GET_PARAMS, KORU_NR_SETUP};

/// Configure the ring. Once per fd, before `mmap`.
pub const KORU_IOC_SETUP: u32 = io_wr::<KoruParams>(KORU_IOC_TYPE, KORU_NR_SETUP);
/// Read the current parameters. Legal before `SETUP`.
pub const KORU_IOC_GET_PARAMS: u32 = io_r::<KoruParams>(KORU_IOC_TYPE, KORU_NR_GET_PARAMS);
/// Submit SQEs and reap CQEs.
pub const KORU_IOC_ENTER: u32 = io_wr::<KoruEnter>(KORU_IOC_TYPE, KORU_NR_ENTER);

// ---------------------------------------------------------------------------
// Thin wrappers
// ---------------------------------------------------------------------------

/// Raw ioctl.
///
/// # Safety
/// `arg` must be valid for whatever the request number says the kernel will
/// read and write.
pub unsafe fn ioctl_raw(fd: BorrowedFd<'_>, request: u32, arg: *mut c_void) -> io::Result<c_int> {
    // SAFETY: the caller guarantees `arg` matches `request`.
    let r = unsafe { ioctl(fd.as_raw_fd(), request as c_ulong, arg) };
    if r < 0 {
        Err(io::Error::last_os_error())
    } else {
        Ok(r)
    }
}

/// `SETUP`. Safe: the request number is derived from `KoruParams`, so the size
/// matches the reference and every bit pattern of the struct is valid.
pub fn ioctl_setup(fd: BorrowedFd<'_>, p: &mut KoruParams) -> io::Result<()> {
    // SAFETY: request and argument type agree by construction.
    unsafe { ioctl_raw(fd, KORU_IOC_SETUP, (p as *mut KoruParams).cast()) }.map(|_| ())
}

/// `GET_PARAMS`. Safe for the same reason.
pub fn ioctl_get_params(fd: BorrowedFd<'_>, p: &mut KoruParams) -> io::Result<()> {
    // SAFETY: request and argument type agree by construction.
    unsafe { ioctl_raw(fd, KORU_IOC_GET_PARAMS, (p as *mut KoruParams).cast()) }.map(|_| ())
}

/// `ENTER`, returning the count of SQEs consumed.
///
/// # Safety
/// `e.sq_addr` and `e.cq_addr` are bare integers the kernel will read and
/// write. Building the struct is safe; handing it over is not.
pub unsafe fn ioctl_enter(fd: BorrowedFd<'_>, e: &mut KoruEnter) -> io::Result<u32> {
    // SAFETY: the caller guarantees the two addresses.
    let r = unsafe { ioctl_raw(fd, KORU_IOC_ENTER, (e as *mut KoruEnter).cast()) }?;
    Ok(r as u32)
}

#[cfg(test)]
mod tests {
    use super::*;

    /// A canary, not a duplicate of the arithmetic above: a struct size change
    /// moves the ioctl number on both sides, which is a wire-format break.
    /// Without this it surfaces only as EPROTO from every ioctl in the VM.
    #[test]
    fn ioctl_numbers_are_what_the_c_header_expands_to() {
        assert_eq!(KORU_IOC_SETUP, 0xc068_6b00);
        assert_eq!(KORU_IOC_GET_PARAMS, 0x8068_6b01);
        assert_eq!(KORU_IOC_ENTER, 0xc040_6b02);
    }

    #[test]
    fn ioc_fields_land_where_the_kernel_looks_for_them() {
        let r = ioc(_IOC_READ | _IOC_WRITE, 'k' as u32, 0x02, 64);
        assert_eq!(r >> _IOC_DIRSHIFT, 3);
        assert_eq!((r >> _IOC_SIZESHIFT) & 0x3fff, 64);
        assert_eq!((r >> _IOC_TYPESHIFT) & 0xff, 'k' as u32);
        assert_eq!(r & 0xff, 0x02);
    }

    #[test]
    fn wait_status_macros() {
        assert!(wifexited(0x0500));
        assert_eq!(wexitstatus(0x0500), 5);
        assert!(!wifexited(9));
        assert!(wifsignaled(9));
        assert_eq!(wtermsig(9), 9);
        assert!(!wifsignaled(0x0500));
    }
}
