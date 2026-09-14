// SPDX-License-Identifier: MIT

//! The three iterators Braam's programs read through: the files named on a
//! command line as one stream, that stream split into lines, and a directory
//! tree walked pre-order.
//!
//! `LineReader` and `TreeWalk` have the same fast-path shape a `File` does:
//! a line already in the buffer, or an entry the walk already listed, must not
//! cost a syscall.

use crate::args::Args;
use crate::future::Handle;
use crate::ops::{self, DirEntry, FileKind};
use crate::vocab::{Error, Kind, Result, Str};
use koru_sys::error::EINVAL;

// ---------------------------------------------------------------------------
// Input
// ---------------------------------------------------------------------------

/// The files named on a command line, read end to end as one stream — `wc a b`.
/// One is open at a time: each is opened when the read reaches it and closed
/// before the next.
///
/// `who` names this program in the diagnostic a failed open prints, and is
/// unused where no path was named.
pub struct Input {
    paths: Args,
    who: String,
    at: usize,
    /// The file `at` names, once opened.
    cur: Option<Handle>,
    /// The fallback, where no path was named.
    fd: Handle,
    own: bool,
}

impl Input {
    pub fn new(paths: Args, fallback: Handle, who: Str<'_>) -> Input {
        let own = !paths.is_empty();
        Input {
            paths,
            who: who.to_string(),
            at: 0,
            cur: None,
            fd: fallback,
            own,
        }
    }

    /// The next chunk of the concatenation, or `Err(Closed)` at the end of it.
    /// A file that will not open is reported here, on stderr, and comes back as
    /// its own error — so a caller's Cancelled-is-130 mapping still holds.
    ///
    /// **Bytes**, for `read_chunk`'s reason: a chunk boundary falls wherever
    /// the read stopped, which may be inside a UTF-8 sequence.
    pub async fn read(&mut self) -> Result<Vec<u8>> {
        if !self.own {
            return ops::read_chunk(self.fd).await;
        }

        loop {
            if self.cur.is_none() {
                if self.at >= self.paths.size() {
                    return Err(Error::closed());
                }
                let path = self.paths[self.at].to_string();
                match ops::open_read(&path).await {
                    Ok(h) => self.cur = Some(h),
                    Err(e) => {
                        // Reported here: the caller has no name for the file,
                        // and maps anything but Closed onto an exit status
                        // without printing.
                        ops::errln(&self.who, &path, e).await;
                        self.at = self.paths.size(); // spent, not to be retried
                        return Err(e);
                    }
                }
            }

            let cur = self.cur.expect("just opened");
            match ops::read_chunk(cur).await {
                Err(e) if e.is(Kind::Closed) => {}
                r => return r,
            }

            // Closed before the next is opened, which is what keeps one open.
            ops::close_fd(cur).await;
            self.cur = None;
            self.at += 1;
        }
    }
}

// ---------------------------------------------------------------------------
// LineReader
// ---------------------------------------------------------------------------

/// Splits an [`Input`] into lines. A line may span any number of chunks, and a
/// final fragment with no newline is a line.
pub struct LineReader {
    src: Input,
    buf: Vec<u8>,
    /// Consumed prefix of `buf`, compacted when it refills.
    pos: usize,
    eof: bool,
}

impl LineReader {
    pub fn new(src: Input) -> LineReader {
        LineReader {
            src,
            buf: Vec::new(),
            pos: 0,
            eof: false,
        }
    }

    /// `out` holds the next line, without its newline. `Ok(false)` past the
    /// last one.
    pub async fn next(&mut self, out: &mut String) -> Result<bool> {
        loop {
            // The fast half: no syscall where the bytes in hand answer.
            if let Some(got) = self.take() {
                out.clear();
                return match String::from_utf8(got) {
                    Ok(s) => {
                        out.push_str(&s);
                        Ok(true)
                    }
                    Err(_) => Err(Error::from_errno(EINVAL)),
                };
            }
            if self.eof {
                out.clear();
                return Ok(false);
            }

            match self.src.read().await {
                Ok(chunk) => {
                    // The unread tail slides down before the buffer takes
                    // more, so a long-running reader does not grow it without
                    // bound.
                    if self.pos > 0 {
                        self.buf.drain(..self.pos);
                        self.pos = 0;
                    }
                    self.buf.extend_from_slice(&chunk);
                }
                Err(e) if e.is(Kind::Closed) => self.eof = true,
                Err(e) => return Err(e),
            }
        }
    }

    /// What the bytes in hand answer: a whole line, or the last fragment once
    /// the input is spent. `None` means "read more".
    fn take(&mut self) -> Option<Vec<u8>> {
        if let Some(i) = self.buf[self.pos..].iter().position(|&b| b == b'\n') {
            let line = self.buf[self.pos..self.pos + i].to_vec();
            self.pos += i + 1;
            if self.pos == self.buf.len() {
                self.buf.clear();
                self.pos = 0;
            }
            return Some(line);
        }
        if !self.eof || self.pos == self.buf.len() {
            return None;
        }
        // A final fragment with no newline is a line.
        let line = self.buf[self.pos..].to_vec();
        self.buf.clear();
        self.pos = 0;
        Some(line)
    }
}

// ---------------------------------------------------------------------------
// TreeWalk
// ---------------------------------------------------------------------------

/// Everything under `root`, pre-order, with an explicit stack rather than
/// recursion: a deep tree must not be a deep chain of coroutine frames.
///
/// Descends on a directory alone, so a link is handed over rather than followed
/// and no cycle guard is needed. `root` itself is not reported — a caller that
/// wants it stats it.
pub struct TreeWalk {
    root: String,
    levels: Vec<Level>,
    /// The directory reported last, listed on the next call.
    pending: Option<String>,
    /// Whichever directory the last error was about.
    at: String,
    began: bool,
}

struct Level {
    ents: Vec<DirEntry>,
    at: usize,
    path: String,
}

impl TreeWalk {
    pub fn new(root: Str<'_>) -> TreeWalk {
        // One trailing slash or several: what is reported is the trimmed root,
        // a '/', and the rest, so root_len splices a destination on.
        let mut r = root;
        while r.len() > 1 && r.ends_with('/') {
            r = &r[..r.len() - 1];
        }
        TreeWalk {
            root: r.to_string(),
            levels: Vec::new(),
            pending: None,
            at: String::new(),
            began: false,
        }
    }

    /// How much of a reported path is the root: `path[root_len()..]` is what
    /// lies under it, leading `/` and all. The root itself contributes none.
    pub fn root_len(&self) -> usize {
        if self.root == "/" { 0 } else { self.root.len() }
    }

    /// Whichever directory the last error was about.
    pub fn at(&self) -> Str<'_> {
        &self.at
    }

    /// `Ok(true)` with `path` the whole path from the root and `e` the entry;
    /// `Ok(false)` past the last one. A directory that will not list is an
    /// error naming it in [`at`](Self::at), and that level is dropped — so a
    /// caller that reports and calls again walks the rest, and one that returns
    /// stops here.
    pub async fn next(&mut self, path: &mut String, e: &mut DirEntry) -> Result<bool> {
        self.at.clear();

        // The fast half: an entry this walk already holds needs no listing.
        if self.began && self.pending.is_none() {
            return Ok(self.report(path, e));
        }

        // The root the first time, then whichever directory was reported last:
        // one listing per call at most, so a failure names one directory.
        let dir = match self.pending.take() {
            Some(p) => p,
            None => self.root.clone(),
        };
        self.began = true;

        let ents = match ops::list_dir(&dir).await {
            Ok(v) => v,
            Err(err) => {
                self.at = dir;
                return Err(err);
            }
        };
        self.levels.push(Level {
            ents,
            at: 0,
            path: dir,
        });
        Ok(self.report(path, e))
    }

    /// One entry off the levels in hand. Deepest first, and a level with
    /// nothing left is popped: an empty directory adds one and loses it again
    /// without reporting anything.
    fn report(&mut self, path: &mut String, out: &mut DirEntry) -> bool {
        while self.levels.last().is_some_and(|l| l.at == l.ents.len()) {
            self.levels.pop();
        }
        let Some(lv) = self.levels.last_mut() else {
            return false;
        };

        let e = lv.ents[lv.at].clone();
        lv.at += 1;
        path.clear();
        path.push_str(&lv.path);
        if !path.ends_with('/') {
            path.push('/');
        }
        path.push_str(&e.name);

        // Descended into on the next call, so the caller sees a directory
        // before what is in it.
        if e.kind == FileKind::Dir {
            self.pending = Some(path.clone());
        }
        *out = e;
        true
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    /// `root_len` splices a destination on, and a trailing slash must not
    /// double it.
    #[test]
    fn a_root_is_trimmed_but_never_to_nothing() {
        assert_eq!(TreeWalk::new("/a/b").root_len(), 4);
        assert_eq!(TreeWalk::new("/a/b/").root_len(), 4);
        assert_eq!(TreeWalk::new("/a/b///").root_len(), 4);
        assert_eq!(
            TreeWalk::new("/").root_len(),
            0,
            "the root contributes none"
        );
    }

    /// The buffer half of `LineReader`, which needs no ring.
    #[test]
    fn lines_come_out_of_the_bytes_in_hand() {
        let mut r = LineReader {
            src: Input::new(Args::new(Vec::new()), Handle(0), ""),
            buf: b"one\ntwo\nthree".to_vec(),
            pos: 0,
            eof: false,
        };
        assert_eq!(r.take(), Some(b"one".to_vec()));
        assert_eq!(r.take(), Some(b"two".to_vec()));
        // No newline behind it and the input is not spent: read more.
        assert_eq!(r.take(), None);
        r.eof = true;
        assert_eq!(r.take(), Some(b"three".to_vec()), "a final fragment");
        assert_eq!(r.take(), None, "and then nothing");
    }

    #[test]
    fn an_empty_line_is_a_line() {
        let mut r = LineReader {
            src: Input::new(Args::new(Vec::new()), Handle(0), ""),
            buf: b"\n\na\n".to_vec(),
            pos: 0,
            eof: true,
        };
        assert_eq!(r.take(), Some(Vec::new()));
        assert_eq!(r.take(), Some(Vec::new()));
        assert_eq!(r.take(), Some(b"a".to_vec()));
        assert_eq!(r.take(), None);
    }
}
