// SPDX-License-Identifier: MIT

//! Braam's `Args`: the argument vector, `name` first.
//!
//! Braam's is a `Span<const Str>` and its `tail` is a subspan, so it costs
//! nothing. Rust owns the strings, so the vector is shared and `tail` moves a
//! start index instead.

use crate::vocab::Str;
use std::ops::Index;
use std::rc::Rc;

#[derive(Clone)]
pub struct Args {
    v: Rc<Vec<String>>,
    start: usize,
}

impl Args {
    /// The process's own, lossy where an argument is not UTF-8 — Braam's `Str`
    /// is UTF-8 by definition and nothing downstream can hold the bytes.
    pub fn from_env() -> Args {
        Args::new(
            std::env::args_os()
                .map(|a| a.to_string_lossy().into_owned())
                .collect(),
        )
    }

    pub fn new(v: Vec<String>) -> Args {
        Args {
            v: Rc::new(v),
            start: 0,
        }
    }

    pub fn size(&self) -> usize {
        self.v.len() - self.start
    }

    pub fn is_empty(&self) -> bool {
        self.size() == 0
    }

    /// Argument 0, the program's own name, or empty where there is none.
    pub fn name(&self) -> Str<'_> {
        if self.is_empty() { "" } else { &self[0] }
    }

    /// Everything but the first, sharing the same vector.
    pub fn tail(&self) -> Args {
        Args {
            v: Rc::clone(&self.v),
            start: (self.start + 1).min(self.v.len()),
        }
    }

    pub fn iter(&self) -> impl Iterator<Item = Str<'_>> {
        self.v[self.start..].iter().map(String::as_str)
    }
}

impl Index<usize> for Args {
    type Output = str;
    fn index(&self, i: usize) -> &str {
        &self.v[self.start + i]
    }
}

impl std::fmt::Debug for Args {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.debug_list().entries(self.iter()).finish()
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn args() -> Args {
        Args::new(vec!["hello".into(), "koru".into(), "world".into()])
    }

    #[test]
    fn name_is_the_first_and_tail_drops_it() {
        let a = args();
        assert_eq!(a.size(), 3);
        assert_eq!(a.name(), "hello");
        assert_eq!(&a[1], "koru");

        let t = a.tail();
        assert_eq!(t.size(), 2);
        assert_eq!(t.name(), "koru");
        assert_eq!(&t[0], "koru");
        assert_eq!(
            a.size(),
            3,
            "tail shares the vector rather than consuming it"
        );
    }

    #[test]
    fn an_empty_vector_has_an_empty_name_and_a_tail_that_stays_empty() {
        let a = Args::new(Vec::new());
        assert!(a.is_empty());
        assert_eq!(a.name(), "");
        assert!(a.tail().is_empty());
        assert!(a.tail().tail().is_empty());
    }

    #[test]
    fn tail_past_the_end_is_empty_rather_than_a_panic() {
        let a = args().tail().tail().tail().tail();
        assert!(a.is_empty());
        assert_eq!(a.name(), "");
    }
}
