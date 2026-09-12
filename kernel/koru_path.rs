// SPDX-License-Identifier: GPL-2.0

//! What the path operations need and `bindings::` does not have, plus the guard
//! types that make an early return through `?` safe.
//!
//! **Nothing checks any of this**: no `static_assert` reaches a C declaration.
//! Every item quotes its header verbatim, and re-reading them is an obligation
//! of every kernel bump. See doc/Notes.md.

use kernel::{
    bindings,
    error::from_err_ptr,
    ffi::{c_char, c_int},
    prelude::*,
    str::CStrExt,
};

// From include/linux/namei.h:
//   extern int kern_path(const char *, unsigned, struct path *);
// `struct path` reaches an empty bindgen struct in this config; the `bindings`
// crate allows the same lint crate-wide.
#[allow(improper_ctypes)]
unsafe extern "C" {
    fn kern_path(name: *const c_char, flags: u32, path: *mut bindings::path) -> c_int;
}

// From include/linux/namei.h:
//   #define LOOKUP_FOLLOW BIT(0)  /* follow links at the end */
pub(crate) const LOOKUP_FOLLOW: u32 = 1 << 0;

/// A resolved `struct path`, `path_put` on drop. A `?` that skipped it would
/// leak a dentry and a vfsmount, and only a refused unmount would say so.
pub(crate) struct Lookup(bindings::path);

impl Lookup {
    /// `kern_path`, in the submitting task: relative paths resolve against its
    /// `current->fs` and components check against its creds. Notes' finding 3.
    pub(crate) fn new(name: &CStr, flags: u32) -> Result<Lookup> {
        let mut p = bindings::path::default();
        // SAFETY: `name` is NUL-terminated and outlives the call, and `p` is
        // our own storage, filled only when this returns 0.
        let ret = unsafe { kern_path(name.as_char_ptr(), flags, &mut p) };
        if ret < 0 {
            return Err(Error::from_errno(ret));
        }
        Ok(Lookup(p))
    }

    pub(crate) fn as_ptr(&self) -> *const bindings::path {
        &self.0
    }

    pub(crate) fn dentry(&self) -> *mut bindings::dentry {
        self.0.dentry
    }
}

impl Drop for Lookup {
    fn drop(&mut self) {
        // SAFETY: `kern_path` filled this path and took its references.
        unsafe { bindings::path_put(&self.0) };
    }
}

/// A symlink target from `vfs_get_link`, with the teardown it owes. Skip the
/// drop and a page-backed symlink leaks a page per readlink; only kmemleak
/// sees it.
pub(crate) struct Link {
    done: bindings::delayed_call,
    target: *const c_char,
}

impl Link {
    /// `vfs_get_link`. `-EINVAL` for a non-symlink, as `readlink(2)` gives.
    pub(crate) fn get(dentry: *mut bindings::dentry) -> Result<Link> {
        let mut link = Link {
            done: bindings::delayed_call::default(),
            target: core::ptr::null(),
        };
        // SAFETY: `dentry` belongs to a live `Lookup` and `done` is ours. On
        // the error path the drop below is still the right one.
        let ptr = unsafe { bindings::vfs_get_link(dentry, &mut link.done) };
        link.target = from_err_ptr(ptr.cast_mut())?;
        Ok(link)
    }

    /// The target, without its NUL.
    pub(crate) fn bytes(&self) -> &[u8] {
        // SAFETY: valid until the delayed call runs, which is this drop.
        unsafe { CStr::from_char_ptr(self.target) }.to_bytes()
    }
}

impl Drop for Link {
    fn drop(&mut self) {
        // `do_delayed_call`, from include/linux/delayed_call.h:
        //   if (call->fn) call->fn(call->arg);
        if let Some(f) = self.done.fn_ {
            // SAFETY: `get_link` set both halves; called once, here.
            unsafe { f(self.done.arg) };
        }
    }
}
