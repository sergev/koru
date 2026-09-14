// SPDX-License-Identifier: MIT

//! A buffered stream over a handle, in place of stdio: a buffer, so a character
//! is not a syscall; runes, so `get` is a codepoint and not a byte; and a
//! sticky error, checked once rather than per character.
//!
//! **Dropping a `File` neither flushes nor closes.** A destructor cannot await,
//! so flushing is `flush`, `close`, or the at-exit hook the standard streams
//! install. That is Braam's promise, and the reason is the same one.
//!
//! This is where the fast path earns its keep, and it earns more here than on
//! Braam: a miss costs an `ENTER` syscall rather than a scheduler step. Every
//! operation answers out of the buffer without suspending where it can — an
//! `async fn` that reaches no `.await` is `Poll::Ready` on the first poll and
//! never reaches the reactor at all.

use crate::filebuf::{FileBuf, LineStep, RuneStep};
use crate::future::Handle;
use crate::iter::Input;
use crate::ops::{self, O_APPEND, O_CREATE, O_READ, O_TRUNC, O_WRITE, SEEK_CUR};
use crate::rt;
use crate::vocab::{Error, Kind, Result, SpanMut, Str};
use koru_sys::error::{EAGAIN, EINVAL, EIO, EOPNOTSUPP};

/// What `open` asks the filesystem for.
#[derive(Copy, Clone, PartialEq, Eq, Debug)]
pub enum FileMode {
    Read,
    /// Truncating, creating.
    Write,
    /// Creating, positioned at the end.
    Append,
    /// Read and write; a direction change costs a flush or a seek.
    Update,
}

#[derive(Copy, Clone, PartialEq, Eq, Debug)]
pub enum Buffering {
    /// Every operation is a syscall.
    None,
    /// Flushed when what was written holds a newline.
    Line,
    /// Flushed when the buffer fills.
    Full,
    /// `Line` on the console, `Full` otherwise; decided on the first flush.
    Auto,
}

/// The block a `File` takes when nothing asks for more.
pub const FILE_BUF: usize = 512;

fn mode_flags(m: FileMode) -> u32 {
    match m {
        FileMode::Read => O_READ,
        FileMode::Write => O_WRITE | O_CREATE | O_TRUNC,
        FileMode::Append => O_WRITE | O_CREATE | O_APPEND,
        FileMode::Update => O_READ | O_WRITE,
    }
}

pub struct File {
    buf: FileBuf,
    want: usize,
    src: Option<Input>,
    fd: Handle,
    mode: FileMode,
    how: Buffering,
    err: Option<Error>,
    own_fd: bool,
    closed: bool,
    /// The buffer holds output rather than input.
    writing: bool,
    /// Flushed before this one refills: a prompt is out before what answers
    /// it is read. Only `stdin` sets it, and only to `stdout`.
    tie_stdout: bool,
}

impl File {
    fn bare(fd: Handle, mode: FileMode) -> File {
        File {
            buf: FileBuf::default(),
            want: FILE_BUF,
            src: None,
            fd,
            mode,
            how: Buffering::Full,
            err: None,
            own_fd: false,
            closed: false,
            writing: false,
            tie_stdout: false,
        }
    }

    pub async fn open(path: Str<'_>, m: FileMode) -> Result<File> {
        let fd = ops::open_at(path, mode_flags(m)).await?;
        let mut f = File::bare(fd, m);
        f.own_fd = true;
        Ok(f)
    }

    /// Wraps a handle this `File` does not own and will not close.
    pub fn of(fd: Handle, m: FileMode) -> File {
        File::bare(fd, m)
    }

    /// Reads the concatenation an `Input` names.
    ///
    /// Braam's takes `Input&` and says it must outlive the `File`; owning it is
    /// how Rust says the same thing, and it cannot be got wrong.
    pub fn over(src: Input) -> File {
        let mut f = File::bare(Handle(0), FileMode::Read);
        f.src = Some(src);
        f
    }

    // -----------------------------------------------------------------------
    // State
    // -----------------------------------------------------------------------

    pub fn fd(&self) -> Handle {
        self.fd
    }

    /// The first error this stream met, or `None`. Braam's sentinel is
    /// `Error(0)`; Rust spells that `Option`.
    pub fn err(&self) -> Option<Error> {
        self.err
    }

    pub fn clean(&self) -> bool {
        self.err.is_none()
    }

    /// An end of input is not a failure.
    pub fn eof(&self) -> bool {
        self.err.is_some_and(|e| e.is(Kind::Closed))
    }

    pub fn failed(&self) -> bool {
        !self.clean() && !self.eof()
    }

    pub fn clear_err(&mut self) {
        self.err = None;
    }

    pub fn set_buffering(&mut self, b: Buffering) {
        self.how = b;
    }

    /// Asks for a larger block than [`FILE_BUF`].
    pub fn reserve(&mut self, n: usize) {
        self.want = n.max(FILE_BUF);
        if self.src.is_none() && self.how != Buffering::None {
            self.block_ready();
        }
    }

    /// Puts a rune back, in front of what is buffered, so `read` and `getline`
    /// see it too. False where there is no room in front.
    pub fn unget(&mut self, c: char) -> bool {
        if self.writing {
            return false;
        }
        self.block_ready();
        self.buf.unget(c)
    }

    /// The first error sticks; a later one does not overwrite it.
    fn fail(&mut self, e: Error) -> Error {
        if self.clean() {
            self.err = Some(e);
        }
        e
    }

    fn readable(&self) -> bool {
        self.src.is_some() || matches!(self.mode, FileMode::Read | FileMode::Update)
    }

    fn writable(&self) -> bool {
        self.src.is_none() && self.mode != FileMode::Read
    }

    fn block_ready(&mut self) {
        if self.src.is_some() {
            return;
        }
        if self.buf.ready() {
            self.buf.regrow(self.want);
        } else {
            self.buf = FileBuf::with_capacity(self.want);
        }
    }

    // -----------------------------------------------------------------------
    // The fast halves: what the buffer alone answers, with no `.await` on any
    // path. A caller that reaches one of these never touches the reactor.
    // -----------------------------------------------------------------------

    fn take_fast(&mut self) -> Option<Result<char>> {
        if let Some(e) = self.err {
            return Some(Err(e));
        }
        if self.writing || !self.buf.ready() {
            return None;
        }
        match self.buf.take() {
            RuneStep::Got(c) => Some(Ok(c)),
            RuneStep::Need => None,
        }
    }

    fn read_fast(&mut self, into: &mut [u8]) -> Option<Result<usize>> {
        if let Some(e) = self.err {
            return Some(Err(e));
        }
        if self.writing || self.buf.is_empty() {
            return None;
        }
        let n = self.buf.size().min(into.len());
        into[..n].copy_from_slice(&self.buf.held()[..n]);
        self.buf.consume(n);
        Some(Ok(n))
    }

    fn put_fast(&mut self, c: char) -> Option<Result<()>> {
        if let Some(e) = self.err {
            return Some(Err(e));
        }
        if !self.writing || !self.buf.ready() || self.buffers_nothing() {
            return None;
        }
        if self.how == Buffering::Line && c == '\n' {
            return None;
        }
        (self.buf.append_rune(c) != 0).then_some(Ok(()))
    }

    fn write_fast(&mut self, s: &[u8]) -> Option<Result<()>> {
        if let Some(e) = self.err {
            return Some(Err(e));
        }
        if !self.writing || !self.buf.ready() || self.buffers_nothing() {
            return None;
        }
        if s.len() > self.buf.room() {
            return None;
        }
        if self.how == Buffering::Line && s.contains(&b'\n') {
            return None;
        }
        self.buf.append(s);
        Some(Ok(()))
    }

    /// A whole line in hand, or nothing taken: `take_line` consumes the
    /// fragment it could not finish, which the slow half would then have to be
    /// told about.
    fn line_fast(&mut self, out: &mut Vec<u8>, keep_nl: bool) -> Option<Result<bool>> {
        if let Some(e) = self.err {
            return Some(if self.eof() { Ok(false) } else { Err(e) });
        }
        if self.writing || !self.buf.has_line() {
            return None;
        }
        out.clear();
        self.buf.take_line(out, keep_nl);
        Some(Ok(true))
    }

    fn buffers_nothing(&self) -> bool {
        self.how == Buffering::None || self.how == Buffering::Auto
    }

    // -----------------------------------------------------------------------
    // The wire
    // -----------------------------------------------------------------------

    async fn fill(&mut self) -> Result<usize> {
        // The tie: a prompt is out before what answers it is read.
        if self.tie_stdout {
            let _ = File::stdout().flush().await;
        }

        if self.src.is_some() {
            // What a rune straddling two chunks left behind: three bytes at
            // most, which is why `Input::read` hands back bytes.
            let carry = self.buf.held().to_vec();
            if carry.len() > 4 {
                return Err(Error::from_errno(EINVAL));
            }
            let src = self.src.as_mut().expect("checked above");
            let chunk = src.read().await?;
            let mut v = carry;
            let had = v.len();
            v.extend_from_slice(&chunk);
            if v.len() <= had {
                return Err(Error::closed());
            }
            self.buf.adopt(v);
            return Ok(self.buf.size() - had);
        }

        self.block_ready();
        self.buf.compact();
        if self.buf.room() == 0 {
            return Err(Error::from_errno(EINVAL));
        }

        let want = self.buf.room() as u32;
        let mut got = Vec::new();
        let n = ops::read_into(self.fd, &mut got, want).await?;
        if n == 0 {
            return Err(Error::closed());
        }
        let n = n.min(self.buf.room());
        self.buf.tail()[..n].copy_from_slice(&got[..n]);
        self.buf.filled(n);
        Ok(n)
    }

    async fn drain(&mut self) -> Result<()> {
        while !self.buf.is_empty() {
            let held = self.buf.held().to_vec();
            let n = ops::write_once(self.fd, &held).await?;
            if n == 0 {
                return Err(Error::from_errno(EIO));
            }
            self.buf.consume(n);
        }
        Ok(())
    }

    pub async fn flush(&mut self) -> Result<()> {
        if !self.writing || self.buf.is_empty() {
            return Ok(());
        }
        match self.drain().await {
            Ok(()) => Ok(()),
            Err(e) => Err(self.fail(e)),
        }
    }

    /// `Auto` decides here, once: a character device is the console.
    async fn probe(&mut self) {
        self.how = if ops::is_console(self.fd).await {
            Buffering::Line
        } else {
            Buffering::Full
        };
    }

    /// Turn the buffer around, flushing or winding back what was in it.
    async fn settle(&mut self, to_write: bool) -> Result<()> {
        if self.how == Buffering::Auto {
            self.probe().await;
        }
        if self.writing == to_write {
            return Ok(());
        }
        if self.writing {
            self.flush().await?;
            self.writing = false;
            return Ok(());
        }

        // Read to write: the read-ahead goes back to the descriptor.
        if !self.buf.is_empty() {
            if self.src.is_some() {
                return Err(Error::from_errno(EOPNOTSUPP));
            }
            let back = -(self.buf.size() as i64);
            if ops::seek_fd(self.fd, back, SEEK_CUR).await.is_err() {
                return Err(Error::from_errno(EOPNOTSUPP));
            }
            self.buf.reset();
        }
        self.writing = true;
        Ok(())
    }

    // -----------------------------------------------------------------------
    // Input
    // -----------------------------------------------------------------------

    /// One rune. `Err(Closed)` at end of input.
    pub async fn get(&mut self) -> Result<char> {
        if let Some(r) = self.take_fast() {
            return r;
        }
        if !self.readable() {
            return Err(self.fail(Error::from_errno(EINVAL)));
        }
        if self.writing
            && let Err(e) = self.settle(false).await
        {
            return Err(self.fail(e));
        }

        loop {
            if self.buf.ready()
                && let RuneStep::Got(c) = self.buf.take()
            {
                return Ok(c);
            }
            if let Err(e) = self.fill().await {
                // A sequence end of input cut short.
                if e.is(Kind::Closed) && self.buf.ready() && !self.buf.is_empty() {
                    return Ok(self.buf.take_broken());
                }
                return Err(self.fail(e));
            }
        }
    }

    /// As many bytes as are there, never more than the span. `Err(Closed)` at
    /// end of input, so a short read is never mistaken for one.
    pub async fn read(&mut self, into: SpanMut<'_, u8>) -> Result<usize> {
        if let Some(r) = self.read_fast(into) {
            return r;
        }
        if !self.readable() {
            return Err(self.fail(Error::from_errno(EINVAL)));
        }
        if self.writing
            && let Err(e) = self.settle(false).await
        {
            return Err(self.fail(e));
        }
        if into.is_empty() {
            return Ok(0);
        }

        // Nothing in hand, and a span bigger than the buffer: read into it.
        if self.buf.is_empty()
            && self.src.is_none()
            && (self.how == Buffering::None || into.len() >= self.want)
        {
            let mut got = Vec::new();
            let want = into.len().min(ops::READ_MAX as usize) as u32;
            return match ops::read_into(self.fd, &mut got, want).await {
                Ok(0) => Err(self.fail(Error::closed())),
                Ok(n) => {
                    let n = n.min(into.len());
                    into[..n].copy_from_slice(&got[..n]);
                    Ok(n)
                }
                Err(e) => Err(self.fail(e)),
            };
        }

        if self.buf.is_empty()
            && let Err(e) = self.fill().await
        {
            return Err(self.fail(e));
        }
        let n = self.buf.size().min(into.len());
        into[..n].copy_from_slice(&self.buf.held()[..n]);
        self.buf.consume(n);
        Ok(n)
    }

    /// One line, without its newline unless `keep_nl`. `Ok(false)` at end of
    /// input; a final fragment with no newline is a line.
    pub async fn getline(&mut self, out: &mut String, keep_nl: bool) -> Result<bool> {
        let mut bytes = Vec::new();
        let got = self.getline_bytes(&mut bytes, keep_nl).await?;
        out.clear();
        if got {
            match String::from_utf8(bytes) {
                Ok(s) => out.push_str(&s),
                Err(_) => return Err(self.fail(Error::from_errno(EINVAL))),
            }
        }
        Ok(got)
    }

    async fn getline_bytes(&mut self, out: &mut Vec<u8>, keep_nl: bool) -> Result<bool> {
        if let Some(r) = self.line_fast(out, keep_nl) {
            return r;
        }
        out.clear();
        if !self.readable() {
            return Err(self.fail(Error::from_errno(EINVAL)));
        }
        if self.writing
            && let Err(e) = self.settle(false).await
        {
            return Err(self.fail(e));
        }

        let mut seen = 0usize;
        loop {
            if self.buf.ready() && !self.buf.is_empty() {
                let was = self.buf.size();
                let step = self.buf.take_line(out, keep_nl);
                seen += was - self.buf.size();
                if step == LineStep::Done {
                    return Ok(true);
                }
            }
            if let Err(e) = self.fill().await {
                // A final fragment with no newline is a line.
                if e.is(Kind::Closed) {
                    self.fail(Error::closed());
                    return Ok(seen != 0);
                }
                return Err(self.fail(e));
            }
        }
    }

    // -----------------------------------------------------------------------
    // Output
    // -----------------------------------------------------------------------

    /// One rune, encoded.
    pub async fn put(&mut self, c: char) -> Result<()> {
        if let Some(r) = self.put_fast(c) {
            return r;
        }
        if !self.writable() {
            return Err(self.fail(Error::from_errno(EINVAL)));
        }
        if let Err(e) = self.settle(true).await {
            return Err(self.fail(e));
        }

        let mut e = [0u8; 4];
        let n = c.encode_utf8(&mut e).len();
        if self.how == Buffering::None {
            return match ops::write_bytes(self.fd, &e[..n]).await {
                Ok(()) => Ok(()),
                Err(err) => Err(self.fail(err)),
            };
        }

        self.block_ready();
        if self.buf.append_rune(c) == 0 {
            self.flush().await?;
            if self.buf.append_rune(c) == 0 {
                return Err(self.fail(Error::from_errno(EINVAL)));
            }
        }
        if self.how == Buffering::Line && c == '\n' {
            self.flush().await?;
        }
        Ok(())
    }

    /// Bytes, not runes: a UTF-8 sequence may straddle two calls.
    pub async fn write(&mut self, s: Str<'_>) -> Result<()> {
        self.write_bytes(s.as_bytes()).await
    }

    pub async fn write_bytes(&mut self, s: &[u8]) -> Result<()> {
        if let Some(r) = self.write_fast(s) {
            return r;
        }
        if !self.writable() {
            return Err(self.fail(Error::from_errno(EINVAL)));
        }
        if let Err(e) = self.settle(true).await {
            return Err(self.fail(e));
        }

        // Longer than the buffer: through it rather than into it.
        if self.how == Buffering::None || s.len() >= self.want {
            self.flush().await?;
            return match ops::write_bytes(self.fd, s).await {
                Ok(()) => Ok(()),
                Err(e) => Err(self.fail(e)),
            };
        }

        self.block_ready();
        if s.len() > self.buf.room() {
            self.flush().await?;
        }
        self.buf.append(s);
        if self.how == Buffering::Line && s.contains(&b'\n') {
            self.flush().await?;
        }
        Ok(())
    }

    // -----------------------------------------------------------------------
    // The rest
    // -----------------------------------------------------------------------

    /// Discards the buffer.
    pub async fn seek(&mut self, off: i64, whence: u32) -> Result<u64> {
        let mut off = off;
        if self.writing {
            self.flush().await?;
        } else if whence == SEEK_CUR {
            // The descriptor is as far ahead as the read-ahead in hand.
            off -= self.buf.size() as i64;
        }
        self.buf.reset();
        self.writing = false;

        match ops::seek_fd(self.fd, off, whence).await {
            Ok(at) => {
                if self.eof() {
                    self.clear_err();
                }
                Ok(at)
            }
            Err(e) => Err(self.fail(e)),
        }
    }

    /// Flushes, then closes if this `File` opened the handle.
    pub async fn close(&mut self) -> Result<()> {
        if self.closed {
            return Ok(());
        }
        self.closed = true;

        let res = self.flush().await;
        if self.own_fd {
            ops::close_fd(self.fd).await;
            self.own_fd = false;
        }
        self.buf.reset();
        self.fail(Error::closed());
        res
    }

    /// Gives the handle back. Unread bytes are wound off a seekable stream,
    /// and are `Err(Unsupported)` on one that is not.
    pub async fn detach(&mut self) -> Result<Handle> {
        if self.src.is_some() || self.closed {
            return Err(Error::from_errno(EOPNOTSUPP));
        }
        if self.writing {
            self.flush().await?;
        } else if !self.buf.is_empty() {
            let back = -(self.buf.size() as i64);
            if ops::seek_fd(self.fd, back, SEEK_CUR).await.is_err() {
                return Err(Error::from_errno(EOPNOTSUPP));
            }
        }
        self.buf.reset();
        self.closed = true;
        self.own_fd = false;
        self.fail(Error::closed());
        Ok(self.fd)
    }

    // -----------------------------------------------------------------------
    // Scanning
    // -----------------------------------------------------------------------
    //
    // `scanf`'s conversions over a stream: one function each rather than a
    // format string, because nothing here takes `...` and a format defeats
    // every check the compiler could make.
    //
    // One byte of lookahead is all these need, and one byte is all the buffer
    // promises to take back: a refill on an `Input`-backed stream carries at
    // most four bytes across, so a scanner that wanted a whole field in hand at
    // once could not have it. They read a byte at a time and stop on the first
    // that does not belong.
    //
    // Two rules, both places where `scanf` is vague. **Leading whitespace
    // follows scanf**: the numeric ones and `scan_token` skip it, `scan_until`
    // does not. And these are **one pass, with no backtracking**: `scan_lit`
    // puts its one byte back on a mismatch, which is all the pushback there is;
    // a `scan_i64` that took whitespace and a sign before finding no digit
    // answers `Err(Invalid)` and does not restore them.

    /// The next byte without consuming it. `Err(Closed)` at end of input.
    async fn peek(&mut self) -> Result<u8> {
        if let Some(e) = self.err {
            return Err(e);
        }
        if !self.readable() {
            return Err(self.fail(Error::from_errno(EINVAL)));
        }
        while !self.buf.ready() || self.buf.is_empty() {
            if let Err(e) = self.fill().await {
                return Err(self.fail(e));
            }
        }
        Ok(self.buf.held()[0])
    }

    pub async fn skip_space(&mut self) -> Result<()> {
        loop {
            match self.peek().await {
                Ok(c) if is_space(c) => self.buf.consume(1),
                Ok(_) => return Ok(()),
                Err(e) if e.is(Kind::Closed) => return Ok(()),
                Err(e) => return Err(e),
            }
        }
    }

    /// The next byte must be `c`; `Ok(false)` and put back where it is not.
    pub async fn scan_lit(&mut self, c: u8) -> Result<bool> {
        match self.peek().await {
            Ok(got) if got == c => {
                self.buf.consume(1);
                Ok(true)
            }
            Ok(_) => Ok(false), // not taken, so nothing to put back
            Err(e) if e.is(Kind::Closed) => Ok(false),
            Err(e) => Err(e),
        }
    }

    /// `scanf`'s `%s`. `Ok(false)` at end of input.
    pub async fn scan_token(&mut self, out: &mut String, width: usize) -> Result<bool> {
        self.skip_space().await?;
        self.scan_run(out, width, |c, _| !is_space(c), &[]).await
    }

    /// `scanf`'s `%[^set]`. `Ok(false)` where the first byte is already in
    /// `stop`.
    pub async fn scan_until(
        &mut self,
        out: &mut String,
        stop: Str<'_>,
        width: usize,
    ) -> Result<bool> {
        self.scan_run(out, width, |c, s| !s.contains(&c), stop.as_bytes())
            .await
    }

    /// The run both token scanners are: bytes while `keep` accepts them.
    async fn scan_run(
        &mut self,
        out: &mut String,
        width: usize,
        keep: fn(u8, &[u8]) -> bool,
        arg: &[u8],
    ) -> Result<bool> {
        let mut got = Vec::new();
        out.clear();
        loop {
            if width != 0 && got.len() >= width {
                break;
            }
            match self.peek().await {
                Ok(c) if keep(c, arg) => {
                    got.push(c);
                    self.buf.consume(1);
                }
                Ok(_) => break,
                Err(e) if e.is(Kind::Closed) => break,
                Err(e) => return Err(e),
            }
        }
        match String::from_utf8(got) {
            Ok(s) => {
                out.push_str(&s);
                Ok(!out.is_empty())
            }
            Err(_) => Err(self.fail(Error::from_errno(EINVAL))),
        }
    }

    /// `scanf`'s `%d %i %u %o %x`; `base` 0 is C's prefix rules.
    pub async fn scan_i64(&mut self, base: u32, width: usize) -> Result<i64> {
        let (v, neg) = self.scan_number(base, width).await?;
        Ok(if neg {
            (v as i64).wrapping_neg()
        } else {
            v as i64
        })
    }

    pub async fn scan_u64(&mut self, base: u32, width: usize) -> Result<u64> {
        let (v, neg) = self.scan_number(base, width).await?;
        Ok(if neg { v.wrapping_neg() } else { v })
    }

    /// The digits, one at a time. `base` 0 reads C's prefix; a `0x` with no hex
    /// digit behind it is `Err(Invalid)` rather than a zero and a pushed-back
    /// `x`, which is the one-pass rule above.
    async fn scan_number(&mut self, base: u32, width: usize) -> Result<(u64, bool)> {
        self.skip_space().await?;

        let mut base = base;
        let mut v = 0u64;
        let mut n = 0usize;
        let mut any = false;
        let mut neg = false;

        let mut c = self.peek_opt().await?;
        if matches!(c, Some(b'-' | b'+')) && (width == 0 || n < width) {
            neg = c == Some(b'-');
            self.buf.consume(1);
            n += 1;
            c = self.peek_opt().await?;
        }

        if c == Some(b'0') && (base == 0 || base == 16 || base == 2) {
            self.buf.consume(1);
            n += 1;
            any = true;
            c = self.peek_opt().await?;
            if matches!(c, Some(b'x' | b'X')) && (base == 0 || base == 16) {
                base = 16;
                any = false;
                self.buf.consume(1);
                n += 1;
                c = self.peek_opt().await?;
            } else if matches!(c, Some(b'b' | b'B')) && base == 0 {
                base = 2;
                any = false;
                self.buf.consume(1);
                n += 1;
                c = self.peek_opt().await?;
            } else if base == 0 {
                base = 8;
            }
        } else if base == 0 {
            base = 10;
        }

        while let Some(ch) = c {
            if width != 0 && n >= width {
                break;
            }
            let Some(d) = digit(ch) else { break };
            if d >= base {
                break;
            }
            v = v.wrapping_mul(u64::from(base)).wrapping_add(u64::from(d));
            self.buf.consume(1);
            n += 1;
            any = true;
            c = self.peek_opt().await?;
        }
        if !any {
            return Err(self.fail(Error::from_errno(EINVAL)));
        }
        Ok((v, neg))
    }

    /// `peek`, with an end of input as `None` rather than an error.
    async fn peek_opt(&mut self) -> Result<Option<u8>> {
        match self.peek().await {
            Ok(c) => Ok(Some(c)),
            Err(e) if e.is(Kind::Closed) => Ok(None),
            Err(e) => Err(e),
        }
    }
}

// ---------------------------------------------------------------------------
// The standard streams
// ---------------------------------------------------------------------------

// Braam's `File::stdin()` hands back a `File&` to a singleton. Rust cannot
// lend out of a thread-local across an `await`, so a [`Std`] names the slot and
// each call borrows the `File` for its own duration.
//
// Two tasks sharing one buffered stream is the program's bug either way — a
// flush that interleaves with an append corrupts the buffer in any language —
// and here it is `Err(Again)` rather than corruption.

use std::cell::RefCell;

thread_local! {
    static STD: RefCell<[Option<File>; 3]> = const { RefCell::new([None, None, None]) };
    static HOOKED: std::cell::Cell<bool> = const { std::cell::Cell::new(false) };
}

/// Forget the cached streams and the at-exit hook. A new ring means new
/// handles, so a `File` built over the old ones names nothing.
pub(crate) fn reset_std() {
    STD.with_borrow_mut(|s| *s = [None, None, None]);
    HOOKED.with(|h| h.set(false));
}

/// One of the three standard streams.
#[derive(Copy, Clone, PartialEq, Eq, Debug)]
pub struct Std(usize);

impl File {
    /// Fully buffered, and tied to `stdout`: a prompt is out before what
    /// answers it is read.
    pub fn stdin() -> Std {
        Std::build(0, rt::stdin(), FileMode::Read, Buffering::Full)
    }

    /// `Buffering::Auto`: line-buffered on the console, fully buffered when
    /// redirected, decided once on the first flush.
    pub fn stdout() -> Std {
        Std::build(1, rt::stdout(), FileMode::Write, Buffering::Auto)
    }

    /// Unbuffered, so a diagnostic is out before whatever follows it.
    pub fn stderr() -> Std {
        Std::build(2, rt::stderr(), FileMode::Write, Buffering::None)
    }
}

impl Std {
    fn build(i: usize, fd: Handle, mode: FileMode, how: Buffering) -> Std {
        STD.with_borrow_mut(|s| {
            if s[i].is_none() {
                let mut f = File::of(fd, mode);
                f.how = how;
                f.tie_stdout = i == 0;
                s[i] = Some(f);
            }
        });
        if i != 0 {
            Std::arm_flush();
        }
        Std(i)
    }

    /// Flushing is `flush`, `close`, or this: a destructor cannot await.
    fn arm_flush() {
        if HOOKED.with(|h| h.replace(true)) {
            return;
        }
        rt::at_exit(|| async {
            for i in 1..3 {
                let _ = Std(i).flush().await;
            }
        });
    }

    /// Takes the `File` out for the length of one call.
    fn take(self) -> Result<File> {
        STD.with_borrow_mut(|s| s[self.0].take())
            .ok_or_else(|| Error::from_errno(EAGAIN))
    }

    fn give(self, f: File) {
        STD.with_borrow_mut(|s| s[self.0] = Some(f));
    }

    /// Reads a state field without taking the stream. `None` where it is in
    /// use, or where this thread has no ambient ring.
    pub fn peek_state<T>(self, f: impl FnOnce(&File) -> T) -> Option<T> {
        STD.with_borrow(|s| s[self.0].as_ref().map(f))
    }

    pub fn set_buffering(self, b: Buffering) {
        STD.with_borrow_mut(|s| {
            if let Some(f) = s[self.0].as_mut() {
                f.set_buffering(b);
            }
        });
    }

    pub fn unget(self, c: char) -> bool {
        STD.with_borrow_mut(|s| s[self.0].as_mut().is_some_and(|f| f.unget(c)))
    }

    pub fn err(self) -> Option<Error> {
        self.peek_state(File::err).flatten()
    }

    pub fn failed(self) -> bool {
        self.peek_state(File::failed).unwrap_or(false)
    }

    pub fn eof(self) -> bool {
        self.peek_state(File::eof).unwrap_or(false)
    }

    pub fn clean(self) -> bool {
        self.peek_state(File::clean).unwrap_or(true)
    }

    pub fn clear_err(self) {
        STD.with_borrow_mut(|s| {
            if let Some(f) = s[self.0].as_mut() {
                f.clear_err();
            }
        });
    }
}

/// Every async operation, forwarded to the borrowed `File`. One arm per
/// method, because a closure returning a future cannot borrow it out.
macro_rules! std_ops {
    ($($name:ident($($a:ident : $t:ty),*) -> $r:ty;)*) => {
        impl Std {
            $(pub async fn $name(self $(, $a: $t)*) -> Result<$r> {
                let mut f = self.take()?;
                let r = f.$name($($a),*).await;
                self.give(f);
                r
            })*
        }
    };
}

std_ops! {
    get() -> char;
    read(into: SpanMut<'_, u8>) -> usize;
    getline(out: &mut String, keep_nl: bool) -> bool;
    put(c: char) -> ();
    write(s: Str<'_>) -> ();
    write_bytes(s: &[u8]) -> ();
    flush() -> ();
    seek(off: i64, whence: u32) -> u64;
    close() -> ();
    skip_space() -> ();
    scan_lit(c: u8) -> bool;
    scan_token(out: &mut String, width: usize) -> bool;
    scan_until(out: &mut String, stop: Str<'_>, width: usize) -> bool;
    scan_i64(base: u32, width: usize) -> i64;
    scan_u64(base: u32, width: usize) -> u64;
}

/// For a port that had `getchar` and `putchar`.
pub async fn get_rune() -> Result<char> {
    File::stdin().get().await
}

pub async fn put_rune(c: char) -> Result<()> {
    File::stdout().put(c).await
}

pub async fn write_out(s: Str<'_>) -> Result<()> {
    File::stdout().write(s).await
}

pub async fn write_err(s: Str<'_>) -> Result<()> {
    File::stderr().write(s).await
}

fn is_space(c: u8) -> bool {
    matches!(c, b' ' | b'\t' | b'\n' | b'\r' | 0x0b | 0x0c)
}

fn digit(c: u8) -> Option<u32> {
    match c {
        b'0'..=b'9' => Some(u32::from(c - b'0')),
        b'a'..=b'z' => Some(u32::from(c - b'a') + 10),
        b'A'..=b'Z' => Some(u32::from(c - b'A') + 10),
        _ => None,
    }
}
