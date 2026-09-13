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

    // Its `_path` sibling is not exported and the exported `_user_path_at` one
    // takes a `char __user *`, so this is the only usable door. It computes the
    // name's hash itself and checks `MAY_EXEC` on the parent.
    //   struct dentry *start_removing(struct mnt_idmap *idmap,
    //                                 struct dentry *parent, struct qstr *name);
    fn start_removing(
        idmap: *mut bindings::mnt_idmap,
        parent: *mut bindings::dentry,
        name: *mut bindings::qstr,
    ) -> *mut bindings::dentry;
}

/// `mnt_idmap`, a static inline. From include/linux/mount.h:
///   /* Pairs with smp_store_release() in do_idmap_mount(). */
///   return READ_ONCE(mnt->mnt_idmap);
fn mnt_idmap(mnt: *mut bindings::vfsmount) -> *mut bindings::mnt_idmap {
    // SAFETY: the caller holds a reference to the mount.
    unsafe { core::ptr::read_volatile(&raw const (*mnt).mnt_idmap) }
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

    pub(crate) fn mnt(&self) -> *mut bindings::vfsmount {
        self.0.mnt
    }

    pub(crate) fn idmap(&self) -> *mut bindings::mnt_idmap {
        mnt_idmap(self.0.mnt)
    }

    /// The resolved directory's inode. Only meaningful on a directory, which
    /// is what `LOOKUP_DIRECTORY` makes sure of.
    pub(crate) fn inode(&self) -> *mut bindings::inode {
        // SAFETY: `kern_path` took a reference to this dentry.
        unsafe { (*self.0.dentry).d_inode }
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

    pub(crate) fn idmap(&self) -> *mut bindings::mnt_idmap {
        mnt_idmap(self.path.mnt)
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

/// The mount's write count, `mnt_drop_write` on drop. **This is the guard the
/// plan asked for at T24 and T26 and that neither needed**: the `vfs_*`
/// wrappers they call take the count themselves. A removal is assembled by
/// hand, so here it is ours, and an unbalanced one pins the filesystem against
/// a read-only remount until reboot.
pub(crate) struct Write(*mut bindings::vfsmount);

impl Write {
    pub(crate) fn want(mnt: *mut bindings::vfsmount) -> Result<Write> {
        // SAFETY: the caller holds a reference to the mount.
        let ret = unsafe { bindings::mnt_want_write(mnt) };
        if ret < 0 {
            return Err(Error::from_errno(ret));
        }
        Ok(Write(mnt))
    }
}

impl Drop for Write {
    fn drop(&mut self) {
        // SAFETY: paired with the `mnt_want_write` above, once.
        unsafe { bindings::mnt_drop_write(self.0) };
    }
}

/// A directory operation: the parent locked and the child looked up,
/// `end_dirop` on drop. `end_dirop` is in `bindings::` because it is declared
/// in fs.h rather than namei.h, which is the one lucky break in this file.
pub(crate) struct Dirop(*mut bindings::dentry);

impl Dirop {
    /// `start_removing`: it hashes the name, refuses `.`, `..`, an empty name
    /// and anything with a separator in it, and checks `MAY_EXEC` on the
    /// parent — all before locking it and looking the child up.
    pub(crate) fn removing(
        idmap: *mut bindings::mnt_idmap,
        parent: *mut bindings::dentry,
        name: &mut bindings::qstr,
    ) -> Result<Dirop> {
        // SAFETY: `parent` belongs to a live `Lookup` and `name` points into a
        // buffer that outlives the call.
        let d = unsafe { start_removing(idmap, parent, name) };
        Ok(Dirop(from_err_ptr(d)?))
    }

    pub(crate) fn dentry(&self) -> *mut bindings::dentry {
        self.0
    }
}

impl Drop for Dirop {
    fn drop(&mut self) {
        // SAFETY: this dentry came from `start_removing`; released once, here.
        unsafe { bindings::end_dirop(self.0) };
    }
}

/// An inode reference, `iput` on drop. `UNLINK` holds one across `end_dirop`,
/// because the last `iput` truncates and that must not happen under the
/// parent's rwsem — the reason `filename_unlinkat` gives in its own comment.
pub(crate) struct Inode(*mut bindings::inode);

impl Inode {
    pub(crate) fn none() -> Inode {
        Inode(core::ptr::null_mut())
    }

    /// `ihold`, or nothing at all for a negative dentry.
    pub(crate) fn hold(&mut self, inode: *mut bindings::inode) {
        if !inode.is_null() {
            // SAFETY: the dirop holds the parent locked, so this inode is live.
            unsafe { bindings::ihold(inode) };
            self.0 = inode;
        }
    }
}

impl Drop for Inode {
    fn drop(&mut self) {
        if !self.0.is_null() {
            // SAFETY: paired with the `ihold` above, once.
            unsafe { bindings::iput(self.0) };
        }
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
