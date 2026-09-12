// SPDX-License-Identifier: MIT

//! A free list over the arena's slots.
//!
//! Advisory only. Exclusivity is kernel-enforced by the `slot_busy` bitmap, and
//! a loser gets `-EBUSY` as a completion.

use crate::ring::Arena;
use std::cell::RefCell;
use std::ops::{Deref, DerefMut};
use std::rc::Rc;

struct PoolInner {
    arena: Arena,
    free: RefCell<Vec<u32>>,
}

pub struct BufPool {
    inner: Rc<PoolInner>,
}

impl BufPool {
    pub fn new(arena: Arena) -> BufPool {
        let free = (0..arena.slot_count()).rev().collect();
        BufPool {
            inner: Rc::new(PoolInner {
                arena,
                free: RefCell::new(free),
            }),
        }
    }

    pub fn acquire(&self) -> Option<BufSlot> {
        let index = self.inner.free.borrow_mut().pop()?;
        Some(BufSlot {
            pool: Rc::clone(&self.inner),
            index,
        })
    }

    pub fn free_count(&self) -> usize {
        self.inner.free.borrow().len()
    }

    pub fn slot_size(&self) -> u32 {
        self.inner.arena.slot_size()
    }

    pub fn slot_count(&self) -> u32 {
        self.inner.arena.slot_count()
    }

    pub fn arena(&self) -> &Arena {
        &self.inner.arena
    }
}

/// One slot, exclusively owned. Move-only: submitting an op moves it into the
/// op state, so no `&mut` can exist while the kernel holds the slot.
pub struct BufSlot {
    pool: Rc<PoolInner>,
    index: u32,
}

impl BufSlot {
    pub fn index(&self) -> u32 {
        self.index
    }

    pub fn len(&self) -> usize {
        self.pool.arena.slot_size() as usize
    }

    pub fn is_empty(&self) -> bool {
        self.len() == 0
    }
}

impl Deref for BufSlot {
    type Target = [u8];
    fn deref(&self) -> &[u8] {
        // SAFETY: owning the BufSlot is exclusive, and an op in flight owns it.
        unsafe { self.pool.arena.slot(self.index) }
    }
}

impl DerefMut for BufSlot {
    fn deref_mut(&mut self) -> &mut [u8] {
        // SAFETY: as above, and &mut self excludes other references.
        unsafe { self.pool.arena.slot_mut(self.index) }
    }
}

impl Drop for BufSlot {
    fn drop(&mut self) {
        self.pool.free.borrow_mut().push(self.index);
    }
}
