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
//   extern struct dentry *start_creating_path(int, const char *, struct path *,
//                                             unsigned int);
//   extern void end_creating_path(const struct path *, struct dentry *);
// `struct path` reaches an empty bindgen struct in this config; the `bindings`
// crate allows the same lint crate-wide.
#[allow(improper_ctypes)]
unsafe extern "C" {
    fn kern_path(name: *const c_char, flags: u32, path: *mut bindings::path) -> c_int;

    fn start_creating_path(
        dfd: c_int,
        name: *const c_char,
        path: *mut bindings::path,
        flags: u32,
    ) -> *mut bindings::dentry;

    fn end_creating_path(path: *const bindings::path, dentry: *mut bindings::dentry);
}

// From include/linux/namei.h:
//   #define LOOKUP_FOLLOW BIT(0)  /* follow links at the end */
//   #define LOOKUP_DIRECTORY BIT(1)  /* require a directory */
pub(crate) const LOOKUP_FOLLOW: u32 = 1 << 0;
pub(crate) const LOOKUP_DIRECTORY: u32 = 1 << 1;

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

/// A `start_creating_path` section: parent inode locked, mount write count
/// taken, a negative dentry to fill. `end_creating_path` on drop puts all three
/// back, so a `?` cannot leave a directory locked for ever.
///
/// **No `mnt_want_write` guard here**: `start_creating_path` takes the write
/// count itself. See doc/Notes.md.
pub(crate) struct Creating {
    path: bindings::path,
    dentry: *mut bindings::dentry,
}

impl Creating {
    /// `start_creating_path` against `current->fs`, in the submitting task.
    pub(crate) fn new(name: &CStr, flags: u32) -> Result<Creating> {
        let mut path = bindings::path::default();
        // SAFETY: `name` is NUL-terminated and outlives the call, and `path` is
        // our own storage, filled only when this returns a real dentry.
        let d = unsafe {
            start_creating_path(
                bindings::AT_FDCWD,
                name.as_char_ptr(),
                &mut path,
                flags,
            )
        };
        Ok(Creating {
            dentry: from_err_ptr(d)?,
            path,
        })
    }

    pub(crate) fn dentry(&self) -> *mut bindings::dentry {
        self.dentry
    }

    /// The parent directory's inode, which the section holds locked.
    pub(crate) fn parent(&self) -> *mut bindings::inode {
        // SAFETY: the section holds a reference to the parent path.
        unsafe { (*self.path.dentry).d_inode }
    }

    /// `mnt_idmap`, a static inline. From include/linux/mount.h:
    ///   /* Pairs with smp_store_release() in do_idmap_mount(). */
    ///   return READ_ONCE(mnt->mnt_idmap);
    pub(crate) fn idmap(&self) -> *mut bindings::mnt_idmap {
        // SAFETY: as above, and the mount outlives this section.
        unsafe { core::ptr::read_volatile(&raw const (*self.path.mnt).mnt_idmap) }
    }

    /// What `vfs_mkdir` returned: it may have replaced the dentry and `dput`
    /// the original, and confusing the two unlocks the wrong inode. An
    /// `ERR_PTR` is fine; `end_dirop` ignores it, as the VFS's own caller does.
    pub(crate) fn replace(&mut self, dentry: *mut bindings::dentry) {
        self.dentry = dentry;
    }
}

impl Drop for Creating {
    fn drop(&mut self) {
        // SAFETY: `start_creating_path` filled the path and returned this
        // dentry, or `vfs_mkdir` replaced it. Called once, here.
        unsafe { end_creating_path(&self.path, self.dentry) };
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
