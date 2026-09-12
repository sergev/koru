// SPDX-License-Identifier: MIT

//! The op slab and its generational key.
//!
//! Generic in its payload, so the mechanics are testable with no device: the
//! production payload owns a `BufSlot`, the tests use a drop counter.

/// A `user_data` value: index low, generation high.
///
/// Not the kernel's handle, which is two `u16`s. The generation is what stops
/// a late completion for a cancelled op from waking whatever reused the index.
#[derive(Copy, Clone, PartialEq, Eq, Hash, Debug)]
pub struct Cookie(pub u64);

impl Cookie {
    pub const fn new(index: u32, generation: u32) -> Cookie {
        Cookie((index as u64) | ((generation as u64) << 32))
    }

    pub const fn index(self) -> u32 {
        self.0 as u32
    }

    pub const fn generation(self) -> u32 {
        (self.0 >> 32) as u32
    }
}

/// The generation after `g`, skipping 0 so no live cookie is ever zero.
const fn bump(g: u32) -> u32 {
    match g.wrapping_add(1) {
        0 => 1,
        n => n,
    }
}

enum Entry<T> {
    Free { generation: u32 },
    Live { generation: u32, value: T },
}

/// A slab keyed by [`Cookie`]. Indices are recycled, generations are not.
pub struct Slab<T> {
    entries: Vec<Entry<T>>,
    free: Vec<u32>,
    live: usize,
}

impl<T> Default for Slab<T> {
    fn default() -> Slab<T> {
        Slab::new()
    }
}

impl<T> Slab<T> {
    pub fn new() -> Slab<T> {
        Slab {
            entries: Vec::new(),
            free: Vec::new(),
            live: 0,
        }
    }

    pub fn len(&self) -> usize {
        self.live
    }

    pub fn insert(&mut self, value: T) -> Cookie {
        self.live += 1;
        if let Some(index) = self.free.pop() {
            let generation = match self.entries[index as usize] {
                Entry::Free { generation } => generation,
                Entry::Live { .. } => unreachable!("free list named a live entry"),
            };
            self.entries[index as usize] = Entry::Live { generation, value };
            return Cookie::new(index, generation);
        }
        let index = self.entries.len() as u32;
        // Generations start at 1, so a valid cookie is never 0.
        self.entries.push(Entry::Live {
            generation: 1,
            value,
        });
        Cookie::new(index, 1)
    }

    fn live_at(&self, c: Cookie) -> bool {
        match self.entries.get(c.index() as usize) {
            Some(Entry::Live { generation, .. }) => *generation == c.generation(),
            _ => false,
        }
    }

    pub fn get(&self, c: Cookie) -> Option<&T> {
        match self.entries.get(c.index() as usize) {
            Some(Entry::Live { generation, value }) if *generation == c.generation() => Some(value),
            _ => None,
        }
    }

    pub fn get_mut(&mut self, c: Cookie) -> Option<&mut T> {
        match self.entries.get_mut(c.index() as usize) {
            Some(Entry::Live { generation, value }) if *generation == c.generation() => Some(value),
            _ => None,
        }
    }

    /// Take the payload out and recycle the index. The caller drops the value
    /// outside any borrow of this slab.
    pub fn remove(&mut self, c: Cookie) -> Option<T> {
        if !self.live_at(c) {
            return None;
        }
        let index = c.index() as usize;
        let slot = &mut self.entries[index];
        let taken = std::mem::replace(
            slot,
            Entry::Free {
                generation: bump(c.generation()),
            },
        );
        self.free.push(c.index());
        self.live -= 1;
        match taken {
            Entry::Live { value, .. } => Some(value),
            Entry::Free { .. } => unreachable!("checked live above"),
        }
    }

    /// Every live payload, the slab left empty and every index retired. The
    /// caller drops them outside any borrow, as `remove` requires.
    pub fn take_all(&mut self) -> Vec<T> {
        let mut out = Vec::with_capacity(self.live);
        for index in 0..self.entries.len() as u32 {
            let generation = match &self.entries[index as usize] {
                Entry::Live { generation, .. } => *generation,
                Entry::Free { .. } => continue,
            };
            if let Some(v) = self.remove(Cookie::new(index, generation)) {
                out.push(v);
            }
        }
        out
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::cell::Cell;
    use std::rc::Rc;

    #[test]
    fn a_cookie_packs_the_index_low_and_the_generation_high() {
        let c = Cookie::new(7, 3);
        assert_eq!(c.0, 0x0000_0003_0000_0007);
        assert_eq!(c.index(), 7);
        assert_eq!(c.generation(), 3);
    }

    /// Mirrors the kernel's handle rule: index 0 generation 1 is not zero.
    #[test]
    fn a_valid_cookie_is_never_zero() {
        let mut s: Slab<u32> = Slab::new();
        let c = s.insert(0);
        assert_eq!(c.index(), 0);
        assert_eq!(c.generation(), 1);
        assert_ne!(c.0, 0);
        assert!(s.get(Cookie(0)).is_none(), "cookie 0 must never resolve");
    }

    #[test]
    fn the_generation_skips_zero_on_wrap() {
        assert_eq!(bump(1), 2);
        assert_eq!(bump(u32::MAX), 1, "0 would make a live cookie look stale");
    }

    #[test]
    fn insert_get_and_remove_round_trip() {
        let mut s: Slab<u32> = Slab::new();
        let a = s.insert(10);
        let b = s.insert(20);
        assert_eq!(s.len(), 2);
        assert_eq!(s.get(a), Some(&10));
        assert_eq!(s.get(b), Some(&20));
        *s.get_mut(a).unwrap() = 11;
        assert_eq!(s.remove(a), Some(11));
        assert_eq!(s.len(), 1);
        assert_eq!(s.get(a), None);
        assert_eq!(s.remove(a), None, "a second remove finds nothing");
    }

    /// The load-bearing one: delete the generation comparison in `live_at`
    /// and this test, and only this test, fails.
    #[test]
    fn a_recycled_index_rejects_the_retired_cookie() {
        let mut s: Slab<u32> = Slab::new();
        let old = s.insert(10);
        assert_eq!(s.remove(old), Some(10));
        let new = s.insert(20);
        assert_eq!(new.index(), old.index(), "the index really was recycled");
        assert_ne!(new.generation(), old.generation());
        assert_eq!(s.get(old), None, "the retired cookie must not resolve");
        assert_eq!(s.get(new), Some(&20));
    }

    #[test]
    fn the_free_list_is_lifo() {
        let mut s: Slab<u32> = Slab::new();
        let a = s.insert(1);
        let b = s.insert(2);
        s.remove(a);
        s.remove(b);
        assert_eq!(s.insert(3).index(), b.index());
        assert_eq!(s.insert(4).index(), a.index());
    }

    #[test]
    fn an_out_of_range_cookie_resolves_to_nothing() {
        let s: Slab<u32> = Slab::new();
        assert_eq!(s.get(Cookie::new(9999, 1)), None);
    }

    #[test]
    fn remove_hands_the_payload_back_rather_than_dropping_it() {
        struct Token(Rc<Cell<u32>>);
        impl Drop for Token {
            fn drop(&mut self) {
                self.0.set(self.0.get() + 1);
            }
        }
        let drops = Rc::new(Cell::new(0));
        let mut s: Slab<Token> = Slab::new();
        let c = s.insert(Token(Rc::clone(&drops)));
        let taken = s.remove(c).expect("live");
        assert_eq!(drops.get(), 0, "remove must not drop under its own borrow");
        drop(taken);
        assert_eq!(drops.get(), 1);
    }
}
