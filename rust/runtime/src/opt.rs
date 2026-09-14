// SPDX-License-Identifier: MIT

//! Braam's `proc/opt.h`: bundled short flags, `--` to end them, and a flag
//! that takes a value. Options come first and the first operand ends them.
//!
//! Allocation-free — every value views the `Args` handed in, which is why the
//! parser borrows one where Braam's holds a span by value.

use crate::args::Args;
use crate::vocab::{Error, Str};
use koru_sys::error::{EINVAL, ENOENT};
use std::fmt;

/// What a program declares: the letters it takes, and which of those consume
/// a value. A valued letter need not appear in `flags`.
#[derive(Copy, Clone, Debug)]
pub struct Opts<'a> {
    pub flags: Str<'a>,  // "1CRSdhlr"
    pub valued: Str<'a>, // "n" — takes the rest of the word, or the next one
}

/// One flag. `value` is empty unless the letter is in [`Opts::valued`].
#[derive(Copy, Clone, Debug, PartialEq, Eq)]
pub struct Opt<'a> {
    pub name: char,
    pub value: Str<'a>,
}

/// A bad command line, and the letter at fault. Braam puts the letter in the
/// out-parameter; here the error carries it, and `?` still yields [`Error`].
#[derive(Copy, Clone, Debug, PartialEq, Eq)]
pub struct OptError {
    pub name: char,
    pub error: Error,
}

impl From<OptError> for Error {
    fn from(e: OptError) -> Error {
        e.error
    }
}

impl fmt::Display for OptError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "-{}: {}", self.name, self.error.kind().name())
    }
}

/// `-h` or `--help` as the whole command line.
pub fn help_asked(args: &Args) -> bool {
    args.size() == 2 && (&args[1] == "-h" || &args[1] == "--help")
}

/// A cursor over argv, starting at argv[1]. The spec is held by value.
pub struct OptParse<'a> {
    args: &'a Args,
    spec: Opts<'a>,
    at: usize,  // the word being read
    in_: usize, // how far into that word's bundle, in bytes; 0 = not started
}

impl<'a> OptParse<'a> {
    pub fn new(args: &'a Args, spec: Opts<'a>) -> OptParse<'a> {
        OptParse {
            args,
            spec,
            at: 1,
            in_: 0,
        }
    }

    /// The next flag, or `None` once the operands begin. `Invalid` is a letter
    /// the program does not take, `NotFound` a valued letter with nothing
    /// after it. Braam's name; not `Iterator`, which has no room for an error.
    #[allow(clippy::should_implement_trait)]
    pub fn next(&mut self) -> Result<Option<Opt<'a>>, OptError> {
        let args = self.args;
        if self.at >= args.size() {
            return Ok(None);
        }

        let w: Str<'a> = &args[self.at];
        if self.in_ == 0 {
            // Anything that is not a flag ends the options, `-` alone included.
            if w.len() < 2 || !w.starts_with('-') {
                return Ok(None);
            }
            if w == "--" {
                self.at += 1;
                return Ok(None);
            }
            self.in_ = 1;
        }

        // A whole rune: a byte, as Braam reads, would slice a value out of
        // mid-sequence and panic.
        let c = match w[self.in_..].chars().next() {
            Some(c) => c,
            None => return Ok(None),
        };
        self.in_ += c.len_utf8();
        let last = self.in_ >= w.len();

        // A valued letter takes the rest of its word, or the next word. Either
        // way it ends the bundle.
        if self.spec.valued.contains(c) {
            let value = if !last {
                self.at += 1;
                &w[self.in_..]
            } else if self.at + 1 < args.size() {
                self.at += 2;
                &args[self.at - 1]
            } else {
                self.at += 1;
                self.in_ = 0;
                return Err(OptError {
                    name: c,
                    error: Error::from_errno(ENOENT),
                });
            };
            self.in_ = 0;
            return Ok(Some(Opt { name: c, value }));
        }

        if last {
            self.at += 1;
            self.in_ = 0;
        }
        // Advanced first, so a caller that carries on does not loop on the
        // letter.
        if !self.spec.flags.contains(c) {
            return Err(OptError {
                name: c,
                error: Error::from_errno(EINVAL),
            });
        }
        Ok(Some(Opt { name: c, value: "" }))
    }

    /// The operands, once `next` has reported `None`.
    pub fn rest(&self) -> Args {
        self.args.skip(self.at)
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::vocab::Kind;

    // ls's letters, and a valued one for head's -n. Braam's own two specs.
    const FLAGS: Opts = Opts {
        flags: "1CRSdhlr",
        valued: "",
    };
    const VALUED: Opts = Opts {
        flags: "v",
        valued: "n",
    };

    /// Braam's own `scan`, string for string: a bare letter, `letter=value`,
    /// `!letter` on an error, then a slash and the operands. One comparison
    /// checks the letters, their order, the values and what is left.
    fn scan(argv: &[&str], spec: Opts<'_>) -> (String, Option<Kind>) {
        let args = Args::new(argv.iter().map(|s| s.to_string()).collect());
        let mut p = OptParse::new(&args, spec);
        let mut out = String::new();
        let mut err = None;

        loop {
            let sep = if out.is_empty() { "" } else { "," };
            match p.next() {
                Err(e) => {
                    err = Some(e.error.kind());
                    out.push_str(sep);
                    out.push('!');
                    out.push(e.name);
                    break;
                }
                Ok(None) => break,
                Ok(Some(o)) => {
                    out.push_str(sep);
                    out.push(o.name);
                    if !o.value.is_empty() {
                        out.push('=');
                        out.push_str(o.value);
                    }
                }
            }
        }

        out.push('/');
        for (i, a) in p.rest().iter().enumerate() {
            if i > 0 {
                out.push(' ');
            }
            out.push_str(a);
        }
        (out, err)
    }

    fn flags(argv: &[&str]) -> String {
        scan(argv, FLAGS).0
    }

    fn valued(argv: &[&str]) -> String {
        scan(argv, VALUED).0
    }

    /// Braam's `test_opt.cpp`, vector for vector.
    #[test]
    fn nothing_at_all_and_operands_with_no_flags() {
        assert_eq!(flags(&["ls"]), "/");
        assert_eq!(flags(&["ls", "a", "b"]), "/a b");
    }

    #[test]
    fn separate_bundled_and_both_read_left_to_right() {
        assert_eq!(flags(&["ls", "-l", "-R", "x"]), "l,R/x");
        assert_eq!(flags(&["ls", "-lR", "x"]), "l,R/x");
        assert_eq!(flags(&["ls", "-lR", "-S", "x"]), "l,R,S/x");
    }

    #[test]
    fn the_first_operand_ends_the_options() {
        assert_eq!(flags(&["ls", "-l", "x", "-R"]), "l/x -R");
    }

    #[test]
    fn a_bare_separator_is_consumed_and_a_lone_dash_is_an_operand() {
        assert_eq!(flags(&["ls", "-l", "--", "-R"]), "l/-R");
        assert_eq!(flags(&["ls", "-", "-l"]), "/- -l");
        assert_eq!(flags(&["ls", "--"]), "/");
    }

    #[test]
    fn a_valued_letter_takes_the_rest_of_its_word_or_the_next_one() {
        assert_eq!(valued(&["head", "-n5", "f"]), "n=5/f");
        assert_eq!(valued(&["head", "-n", "5", "f"]), "n=5/f");
        // It ends the bundle: what follows the letter is the value.
        assert_eq!(valued(&["head", "-vn12", "f"]), "v,n=12/f");
        assert_eq!(valued(&["head", "-vn", "12", "f"]), "v,n=12/f");
        // The operand after a detached value is an operand, not a bundle:
        // Braam's vectors all name a one-letter file and cannot say it.
        assert_eq!(valued(&["head", "-n", "5", "file"]), "n=5/file");
        assert_eq!(valued(&["head", "-vn", "12", "file", "x"]), "v,n=12/file x");
    }

    #[test]
    fn a_missing_argument_and_an_unknown_letter_are_two_different_mistakes() {
        assert_eq!(
            scan(&["head", "-n"], VALUED),
            ("!n/".into(), Some(Kind::NotFound))
        );
        assert_eq!(
            scan(&["ls", "-z", "x"], FLAGS),
            ("!z/x".into(), Some(Kind::Invalid))
        );
        // From inside a bundle, and what was read before it still came out.
        assert_eq!(
            scan(&["ls", "-lz", "x"], FLAGS),
            ("l,!z/x".into(), Some(Kind::Invalid))
        );
    }

    /// Braam's "advanced first": a program that reports a bad letter and
    /// carries on reaches the operands rather than looping on the letter.
    #[test]
    fn a_caller_that_carries_on_past_an_error_still_terminates() {
        let args = Args::new(
            ["ls", "-lzq", "-z", "x"]
                .iter()
                .map(|s| s.to_string())
                .collect(),
        );
        let mut p = OptParse::new(&args, FLAGS);
        let mut bad = Vec::new();
        let mut good = Vec::new();

        for _ in 0..16 {
            match p.next() {
                Err(e) => bad.push(e.name),
                Ok(None) => break,
                Ok(Some(o)) => good.push(o.name),
            }
        }
        assert_eq!(good, ['l']);
        assert_eq!(bad, ['z', 'q', 'z'], "a letter was repeated or skipped");
        assert_eq!(p.rest().size(), 1);
        assert_eq!(&p.rest()[0], "x");
    }

    /// The error is an `Error` through `?`, and the letter survives beside it.
    #[test]
    fn an_error_converts_to_the_vocabularys_own() {
        fn parse(args: &Args) -> crate::vocab::Result<usize> {
            let mut p = OptParse::new(args, FLAGS);
            let mut n = 0;
            while p.next()?.is_some() {
                n += 1;
            }
            Ok(n)
        }

        let ok = Args::new(vec!["ls".into(), "-lR".into(), "x".into()]);
        assert_eq!(parse(&ok).unwrap(), 2);

        let bad = Args::new(vec!["ls".into(), "-lz".into()]);
        assert_eq!(parse(&bad).unwrap_err().kind(), Kind::Invalid);
    }

    /// A letter outside ASCII is one the program does not take, not a panic
    /// on a slice boundary.
    #[test]
    fn a_multibyte_letter_is_refused_whole() {
        let (out, err) = scan(&["ls", "-é", "x"], VALUED);
        assert_eq!(out, "!é/x");
        assert_eq!(err, Some(Kind::Invalid));

        let spec = Opts {
            flags: "",
            valued: "é",
        };
        assert_eq!(scan(&["ls", "-é5", "x"], spec).0, "é=5/x");
    }

    #[test]
    fn help_is_asked_only_as_the_whole_line() {
        let line = |v: &[&str]| Args::new(v.iter().map(|s| s.to_string()).collect());

        assert!(help_asked(&line(&["rm", "-h"])));
        assert!(help_asked(&line(&["rm", "--help"])));
        assert!(!help_asked(&line(&["rm"])));
        // A file named `-h` is still an operand, and a value is never argv[1].
        assert!(!help_asked(&line(&["rm", "-h", "x"])));
        assert!(!help_asked(&line(&["basename", "-s", "-h", "x"])));
        // Neither is any other spelling of it.
        assert!(!help_asked(&line(&["rm", "-help"])));
        assert!(!help_asked(&line(&["rm", "--h"])));
    }
}
