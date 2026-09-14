// SPDX-License-Identifier: MIT

//! The half of a buffered stream that performs no syscall: the bytes in hand,
//! the rune boundaries in them, and the room left. A [`File`](crate::File) owns
//! one, and this is where its fast path lives.
//!
//! One buffer serves both directions: `held` is what a reader has not taken or
//! a writer has not sent, `room` what may follow. Braam lends the block from
//! its allocator; here the buffer owns a `Vec`, and an `Input` chunk is **moved
//! in** rather than copied, which is the same zero-copy the lending buys.

/// What [`FileBuf::take`] did.
#[derive(Copy, Clone, PartialEq, Eq, Debug)]
pub enum RuneStep {
    /// `out` holds a codepoint.
    Got(char),
    /// The bytes in hand do not finish one.
    Need,
}

/// What [`FileBuf::take_line`] did.
#[derive(Copy, Clone, PartialEq, Eq, Debug)]
pub enum LineStep {
    /// A newline was found and `out` holds the line.
    Done,
    /// `out` holds a fragment; there are more bytes to come.
    Need,
}

#[derive(Default)]
pub struct FileBuf {
    b: Vec<u8>,
    pos: usize, // taken prefix of the block
    len: usize, // one past the last byte held
}

impl FileBuf {
    /// A block of `cap` bytes, empty.
    pub fn with_capacity(cap: usize) -> FileBuf {
        FileBuf {
            b: vec![0; cap],
            pos: 0,
            len: 0,
        }
    }

    /// Takes `v` whole, every byte of it held. There is no room to append
    /// after this: it is a view of somebody else's chunk, as Braam's is.
    pub fn adopt(&mut self, v: Vec<u8>) {
        self.len = v.len();
        self.b = v;
        self.pos = 0;
    }

    /// Grow to `cap`, keeping what is held. False where it already is that big.
    pub fn regrow(&mut self, cap: usize) -> bool {
        if self.b.len() >= cap {
            return false;
        }
        self.compact();
        self.b.resize(cap, 0);
        true
    }

    pub fn ready(&self) -> bool {
        !self.b.is_empty()
    }

    pub fn size(&self) -> usize {
        self.len - self.pos
    }

    pub fn is_empty(&self) -> bool {
        self.pos == self.len
    }

    /// What a reader has not taken, or a writer has not sent.
    pub fn held(&self) -> &[u8] {
        &self.b[self.pos..self.len]
    }

    pub fn consume(&mut self, n: usize) {
        self.pos += n.min(self.size());
        if self.pos == self.len {
            self.pos = 0;
            self.len = 0;
        }
    }

    /// Moves the held bytes to the front.
    pub fn compact(&mut self) {
        if self.pos == 0 {
            return;
        }
        self.b.copy_within(self.pos..self.len, 0);
        self.len -= self.pos;
        self.pos = 0;
    }

    /// Where the next fill lands, and how much of it there is room for.
    pub fn tail(&mut self) -> &mut [u8] {
        let len = self.len;
        &mut self.b[len..]
    }

    pub fn room(&self) -> usize {
        self.b.len() - self.len
    }

    pub fn filled(&mut self, n: usize) {
        self.len += n;
    }

    pub fn reset(&mut self) {
        self.pos = 0;
        self.len = 0;
    }

    // -----------------------------------------------------------------------
    // Reading
    // -----------------------------------------------------------------------

    pub fn take(&mut self) -> RuneStep {
        match utf8_decode(self.held()) {
            0 => RuneStep::Need,
            n => {
                let ch = decode_one(self.held());
                self.consume(n);
                RuneStep::Got(ch)
            }
        }
    }

    /// Consumes one byte and yields U+FFFD: a sequence end of input cut short.
    pub fn take_broken(&mut self) -> char {
        if !self.is_empty() {
            self.consume(1);
        }
        '\u{fffd}'
    }

    /// Puts `c` back, in front of the held bytes. False where there is no room.
    pub fn unget(&mut self, c: char) -> bool {
        let mut e = [0u8; 4];
        let n = c.encode_utf8(&mut e).len();

        // Room in front, made by moving the held bytes along.
        if self.pos < n {
            if self.size() + n > self.b.len() {
                return false;
            }
            self.b.copy_within(self.pos..self.len, n);
            self.len = self.size() + n;
            self.pos = n;
        }
        self.pos -= n;
        self.b[self.pos..self.pos + n].copy_from_slice(&e[..n]);
        true
    }

    /// Appends the held bytes up to a newline, consuming what it appended.
    pub fn take_line(&mut self, out: &mut Vec<u8>, keep_nl: bool) -> LineStep {
        match self.held().iter().position(|&b| b == b'\n') {
            None => {
                let n = self.size();
                out.extend_from_slice(self.held());
                self.consume(n);
                LineStep::Need
            }
            Some(nl) => {
                let upto = if keep_nl { nl + 1 } else { nl };
                out.extend_from_slice(&self.held()[..upto]);
                self.consume(nl + 1);
                LineStep::Done
            }
        }
    }

    /// Whether a whole line is in hand: what `take_line` answers `Done` to.
    pub fn has_line(&self) -> bool {
        self.ready() && self.held().contains(&b'\n')
    }

    // -----------------------------------------------------------------------
    // Writing
    // -----------------------------------------------------------------------

    pub fn append(&mut self, s: &[u8]) -> bool {
        if s.len() > self.room() {
            return false;
        }
        self.b[self.len..self.len + s.len()].copy_from_slice(s);
        self.len += s.len();
        true
    }

    /// The bytes the encoding took, or 0 where it would not fit.
    pub fn append_rune(&mut self, c: char) -> usize {
        let mut e = [0u8; 4];
        let n = c.encode_utf8(&mut e).len();
        if self.append(&e[..n]) { n } else { 0 }
    }
}

// ---------------------------------------------------------------------------
// UTF-8
// ---------------------------------------------------------------------------

/// The bytes the sequence at the front of `s` takes, or 0 where it runs past
/// the end. **Every malformed sequence is one byte and U+FFFD**, so bad input
/// is visible rather than silently dropped — Braam's `utf8_decode`'s rule.
///
/// Rust's `char` cannot hold a surrogate or a value past U+10FFFF, so the two
/// cases that need an explicit test there are unrepresentable here.
pub fn utf8_decode(s: &[u8]) -> usize {
    let Some(&lead) = s.first() else { return 0 };
    let want = match lead {
        0x00..=0x7f => return 1,
        0xc2..=0xdf => 2,
        0xe0..=0xef => 3,
        0xf0..=0xf4 => 4,
        // A stray continuation byte, or a lead that cannot start one.
        _ => return 1,
    };
    if s.len() < want {
        // Only a *prefix* of a valid sequence may ask for more bytes; a
        // truncated run whose continuations are already wrong is malformed now.
        return if s[1..].iter().all(|&b| is_cont(b)) {
            0
        } else {
            1
        };
    }
    match std::str::from_utf8(&s[..want]) {
        Ok(_) => want,
        Err(_) => 1, // overlong, surrogate, or a missing continuation
    }
}

/// The codepoint `utf8_decode` measured. Separate so the measuring is pure.
fn decode_one(s: &[u8]) -> char {
    let n = utf8_decode(s);
    if n == 0 {
        return '\u{fffd}';
    }
    std::str::from_utf8(&s[..n])
        .ok()
        .and_then(|t| t.chars().next())
        .unwrap_or('\u{fffd}')
}

fn is_cont(b: u8) -> bool {
    b & 0xc0 == 0x80
}

#[cfg(test)]
mod tests {
    use super::*;

    fn filled(s: &str, cap: usize) -> FileBuf {
        let mut b = FileBuf::with_capacity(cap);
        assert!(b.append(s.as_bytes()));
        b
    }

    #[test]
    fn held_shrinks_as_it_is_consumed_and_resets_when_it_empties() {
        let mut b = filled("abcdef", 16);
        assert_eq!(b.size(), 6);
        assert_eq!(b.held(), b"abcdef");
        b.consume(2);
        assert_eq!(b.held(), b"cdef");
        // Consuming past the end takes what is there and no more.
        b.consume(99);
        assert!(b.is_empty());
        assert_eq!(b.room(), 16, "and the block is whole again");
    }

    #[test]
    fn compact_moves_the_held_bytes_to_the_front() {
        let mut b = filled("abcdef", 8);
        b.consume(4);
        assert_eq!(b.room(), 2);
        b.compact();
        assert_eq!(b.held(), b"ef");
        assert_eq!(b.room(), 6);
    }

    #[test]
    fn a_rune_comes_out_whole_or_not_at_all() {
        let mut b = filled("aé☃", 16);
        assert_eq!(b.take(), RuneStep::Got('a'));
        assert_eq!(b.take(), RuneStep::Got('é'));
        assert_eq!(b.take(), RuneStep::Got('☃'));
        assert_eq!(b.take(), RuneStep::Need, "nothing left is not a rune");

        // A sequence cut short asks for more rather than guessing.
        let mut b = FileBuf::with_capacity(16);
        assert!(b.append(&"☃".as_bytes()[..2]));
        assert_eq!(b.take(), RuneStep::Need);
        assert!(b.append(&"☃".as_bytes()[2..]));
        assert_eq!(b.take(), RuneStep::Got('☃'));
    }

    /// Braam's rule: every malformed sequence is one byte and U+FFFD.
    #[test]
    fn every_malformed_sequence_is_one_byte_and_a_replacement() {
        for bad in [
            &[0x80u8][..],           // a stray continuation
            &[0xff][..],             // a lead that cannot start one
            &[0xc0, 0x80][..],       // overlong
            &[0xed, 0xa0, 0x80][..], // a surrogate
            &[0xe2, 0x28, 0xa1][..], // a missing continuation
            &[0xf5, 0x80, 0x80][..], // past U+10FFFF
        ] {
            let mut b = FileBuf::with_capacity(16);
            assert!(b.append(bad));
            let was = b.size();
            assert_eq!(b.take(), RuneStep::Got('\u{fffd}'), "{bad:?}");
            assert_eq!(b.size(), was - 1, "one byte taken: {bad:?}");
        }
    }

    #[test]
    fn take_broken_is_one_byte_and_a_replacement_even_at_the_end() {
        let mut b = FileBuf::with_capacity(16);
        assert!(b.append(&"☃".as_bytes()[..1]));
        assert_eq!(b.take(), RuneStep::Need);
        assert_eq!(b.take_broken(), '\u{fffd}');
        assert!(b.is_empty());
        // An empty buffer still answers, and consumes nothing.
        assert_eq!(b.take_broken(), '\u{fffd}');
    }

    #[test]
    fn unget_goes_in_front_of_what_is_held() {
        let mut b = filled("bc", 16);
        assert!(b.unget('a'));
        assert_eq!(b.held(), b"abc");
        assert_eq!(b.take(), RuneStep::Got('a'));

        // A multi-byte rune, and one that will not fit.
        assert!(b.unget('☃'));
        assert_eq!(b.held(), "☃bc".as_bytes());
        let mut tight = filled("xy", 3);
        assert!(!tight.unget('☃'), "no room in front or behind");
        assert_eq!(tight.held(), b"xy", "and nothing was disturbed");
    }

    #[test]
    fn a_line_is_taken_whole_or_left_as_a_fragment() {
        let mut b = filled("one\ntwo", 16);
        let mut out = Vec::new();
        assert_eq!(b.take_line(&mut out, false), LineStep::Done);
        assert_eq!(out, b"one");
        assert!(b.has_line() == false, "what is left has no newline");

        out.clear();
        assert_eq!(b.take_line(&mut out, false), LineStep::Need);
        assert_eq!(out, b"two");
        assert!(b.is_empty(), "the fragment was consumed too");

        let mut b = filled("one\n", 16);
        let mut out = Vec::new();
        assert_eq!(b.take_line(&mut out, true), LineStep::Done);
        assert_eq!(out, b"one\n", "keep_nl keeps it");
    }

    #[test]
    fn append_refuses_what_will_not_fit_rather_than_truncating() {
        let mut b = FileBuf::with_capacity(4);
        assert!(b.append(b"abc"));
        assert!(!b.append(b"de"));
        assert_eq!(b.held(), b"abc");
        assert_eq!(b.append_rune('d'), 1);
        assert_eq!(b.append_rune('e'), 0, "full");
        assert_eq!(b.held(), b"abcd");

        let mut b = FileBuf::with_capacity(4);
        assert_eq!(b.append_rune('☃'), 3);
        assert_eq!(b.append_rune('☃'), 0, "three more would not fit");
    }

    /// An `Input` chunk moves in whole, with no room behind it.
    #[test]
    fn an_adopted_chunk_is_held_entirely_and_has_no_room() {
        let mut b = FileBuf::with_capacity(8);
        b.adopt(b"hello".to_vec());
        assert_eq!(b.held(), b"hello");
        assert_eq!(b.room(), 0, "no room behind somebody else's chunk");
    }

    #[test]
    fn regrow_keeps_what_is_held() {
        let mut b = filled("abcdef", 8);
        b.consume(2);
        assert!(b.regrow(32));
        assert_eq!(b.held(), b"cdef");
        assert_eq!(b.room(), 28);
        assert!(!b.regrow(16), "already bigger");
    }
}
