// SPDX-License-Identifier: MIT

//! Braam's vocabulary in Rust: the fifteen names, `Result`, and the aliases
//! Rust spells differently. Not a second `Error` — koru-sys's, re-exported.
//! The `?` conversions are in koru-sys, where the orphan rule puts them.
//! doc/Notes.md has the reasoning.

pub use koru_sys::error::{Errno, Error, KINDS, Kind};

/// Braam's `Result<T, E = Error>`. `TRY` and `CO_TRY` are Rust's `?`.
pub type Result<T, E = Error> = core::result::Result<T, E>;

/// Braam's `Str`: a non-owning view of UTF-8 bytes.
pub type Str<'a> = &'a str;

/// Braam's `Span<const T>`. `String`, `Option` and the integer names are
/// Rust's own, so these three are the whole alias list.
pub type Span<'a, T> = &'a [T];

/// Braam's `Span<T>`, the mutable one.
pub type SpanMut<'a, T> = &'a mut [T];

#[cfg(test)]
mod tests {
    use super::*;
    use koru_sys::EnterError;
    use koru_sys::error::{EPIPE, KORU_ERRNOS, kind_of};
    use std::collections::{HashMap, HashSet};
    use std::io;

    /// Takes koru-sys's `Error` by its own path: a second type defined here
    /// would not compile.
    fn as_sys(e: koru_sys::error::Error) -> koru_sys::error::Kind {
        e.kind()
    }

    #[test]
    fn the_vocabulary_is_koru_syss_types_re_exported() {
        let e: Error = Errno(22).into();
        assert_eq!(as_sys(e), Kind::Invalid);
    }

    /// The done test, over `KORU_ERRNOS` unchanged.
    #[test]
    fn every_errno_maps_to_exactly_one_name_and_back_to_its_raw_value() {
        let mut seen: HashMap<i32, Kind> = HashMap::new();
        for d in KORU_ERRNOS {
            let e = Error::from(d.errno);
            assert_eq!(e.kind(), d.kind, "{} took the wrong name", d.name);
            assert_eq!(e.raw(), d.errno, "{} lost its raw errno", d.name);
            assert!(e.is(d.kind));
            assert_eq!(e, d.kind, "{} fails the bare-name comparison", d.name);
            assert_eq!(kind_of(d.errno), d.kind);
            assert!(
                seen.insert(d.errno.0, d.kind).is_none(),
                "{} appears twice",
                d.name
            );
        }
        assert_eq!(seen.len(), KORU_ERRNOS.len());
    }

    /// `?` from each error type a koru program meets. A missing `From` fails
    /// this at compile time, which is why the loop asserts so little.
    #[test]
    fn the_question_mark_converts_every_errno_koru_can_produce() {
        fn from_errno(e: Errno) -> Result<()> {
            Err(e)?;
            Ok(())
        }
        fn from_io(e: io::Error) -> Result<()> {
            Err(e)?;
            Ok(())
        }
        fn from_enter(e: EnterError) -> Result<()> {
            Err(e)?;
            Ok(())
        }

        for d in KORU_ERRNOS {
            let want = Error::from(d.errno);
            assert_eq!(from_errno(d.errno).unwrap_err(), want);
            assert_eq!(from_io(d.errno.as_io()).unwrap_err(), want);
            let entered = EnterError {
                errno: d.errno,
                progress: Default::default(),
            };
            assert_eq!(from_enter(entered).unwrap_err(), want);
        }
    }

    /// Back to a raw errno through an ordinary Rust caller.
    #[test]
    fn every_error_round_trips_through_io_error() {
        for d in KORU_ERRNOS {
            let e = Error::from(d.errno);
            let io: io::Error = e.into();
            assert_eq!(io.raw_os_error(), Some(d.errno.0), "{}", d.name);
            assert_eq!(Error::from(io), e, "{}", d.name);
        }
    }

    /// A name no errno reaches is a name no program can be handed. Since T21
    /// there are none: `Closed` arrives as EPIPE from a write, and end of file
    /// synthesises it with no errno at all.
    #[test]
    fn every_name_is_reachable_from_the_errno_table() {
        let mapped: HashSet<Kind> = KORU_ERRNOS.iter().map(|d| d.kind).collect();
        let orphans: Vec<Kind> = KINDS
            .iter()
            .copied()
            .filter(|k| !mapped.contains(k))
            .collect();
        assert_eq!(orphans, [] as [Kind; 0], "a name no errno can produce");
        assert_eq!(Error::from(EPIPE).kind(), Kind::Closed);
        assert_eq!(Error::closed().kind(), Kind::Closed);
        assert_eq!(Error::closed().raw(), Errno(0), "end of file has no errno");
    }

    #[test]
    fn the_aliases_spell_braams_signatures() {
        fn first(s: Str<'_>) -> Option<char> {
            s.chars().next()
        }
        fn total(xs: Span<'_, u8>) -> u32 {
            xs.iter().map(|&b| b as u32).sum()
        }
        fn zero(xs: SpanMut<'_, u8>) {
            xs.fill(0);
        }

        let mut buf = [1u8, 2, 3];
        assert_eq!(first("koru"), Some('k'));
        assert_eq!(total(&buf), 6);
        zero(&mut buf);
        assert_eq!(total(&buf), 0);
    }
}
