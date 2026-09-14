// SPDX-License-Identifier: MIT

#include <koru/file.hpp>

#include <cerrno>
#include <cstring>

namespace koru {

namespace {

result<void> ok()
{
    return result<void>();
}

Error mk_err(int e)
{
    return Error::from_errno(Errno(e));
}

Span<u8> bytes_of(Str s)
{
    return Span<u8>(reinterpret_cast<const u8 *>(s.data()), s.size());
}

bool is_space(u8 c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == 0x0b || c == 0x0c;
}

bool digit(u8 c, u32 &out)
{
    if (c >= '0' && c <= '9')
        out = u32(c - '0');
    else if (c >= 'a' && c <= 'z')
        out = u32(c - 'a') + 10;
    else if (c >= 'A' && c <= 'Z')
        out = u32(c - 'A') + 10;
    else
        return false;
    return true;
}

u32 mode_flags(FileMode m)
{
    switch (m) {
    case FileMode::Read:
        return O_READ;
    case FileMode::Write:
        return O_WRITE | O_CREATE | O_TRUNC;
    case FileMode::Append:
        return O_WRITE | O_CREATE | O_APPEND;
    case FileMode::Update:
        return O_READ | O_WRITE;
    }
    return O_READ;
}

// The three standard streams, and whether the at-exit flush is armed. A new
// ring means new handles, so `install` clears both through `reset_std`.
Option<File> &std_slot(int i)
{
    static Option<File> slots[3];
    return slots[i];
}

bool &hooked()
{
    static bool h = false;
    return h;
}

/// Flushing is `flush`, `close`, or this: a destructor cannot await.
void arm_flush()
{
    if (hooked())
        return;
    hooked() = true;
    at_exit([]() -> task<void> {
        for (int i = 1; i < 3; i++)
            if (std_slot(i))
                co_await std_slot(i)->flush();
    });
}

File &std_stream(int i, Handle fd, FileMode mode, Buffering how)
{
    Option<File> &slot = std_slot(i);
    if (!slot) {
        slot = File::of(fd, mode);
        slot->set_buffering(how);
    }
    if (i != 0)
        arm_flush();
    return *slot;
}

} // namespace

namespace detail {

void reset_std()
{
    for (int i = 0; i < 3; i++)
        std_slot(i).reset();
    hooked() = false;
}

} // namespace detail

// ---------------------------------------------------------------------------
// Construction and state
// ---------------------------------------------------------------------------

File File::bare(Handle fd, FileMode m)
{
    File f;
    f.fd_   = fd;
    f.mode_ = m;
    return f;
}

task<result<File>> File::open(Str path, FileMode m)
{
    CO_LET(fd, co_await open_at(path, mode_flags(m)));
    File f    = bare(fd, m);
    f.own_fd_ = true;
    co_return std::move(f);
}

File File::of(Handle fd, FileMode m)
{
    return bare(fd, m);
}

File File::over(Input src)
{
    File f = bare(0, FileMode::Read);
    f.src_ = std::move(src);
    return f;
}

/// Fully buffered, and tied to `out()`: a prompt is out before what answers it
/// is read.
File &File::in()
{
    File &f    = std_stream(0, in_fd(), FileMode::Read, Buffering::Full);
    f.tie_out_ = true;
    return f;
}

/// `Buffering::Auto`: line-buffered on the console, fully buffered when
/// redirected, decided once on the first flush.
File &File::out()
{
    return std_stream(1, out_fd(), FileMode::Write, Buffering::Auto);
}

/// Unbuffered, so a diagnostic is out before whatever follows it.
File &File::err()
{
    return std_stream(2, err_fd(), FileMode::Write, Buffering::None);
}

Error File::fail_with(Error e)
{
    if (clean())
        err_ = e;
    return e;
}

bool File::readable() const
{
    return src_.has_value() || mode_ == FileMode::Read || mode_ == FileMode::Update;
}

bool File::writable() const
{
    return !src_.has_value() && mode_ != FileMode::Read;
}

void File::block_ready()
{
    if (src_)
        return;
    if (buf_.ready())
        buf_.regrow(want_);
    else
        buf_ = FileBuf::with_capacity(want_);
}

void File::reserve(size_t n)
{
    want_ = n > FILE_BUF ? n : FILE_BUF;
    if (!src_ && how_ != Buffering::None)
        block_ready();
}

bool File::unget(u32 c)
{
    if (writing_)
        return false;
    block_ready();
    return buf_.unget(c);
}

// ---------------------------------------------------------------------------
// The fast halves
// ---------------------------------------------------------------------------

bool File::take_fast(result<u32> &out)
{
    if (err_) {
        out = *err_;
        return true;
    }
    if (writing_ || !buf_.ready())
        return false;
    u32 c = 0;
    if (buf_.take(c) == RuneStep::Need)
        return false;
    out = c;
    return true;
}

bool File::read_fast(SpanMut<u8> into, result<size_t> &out)
{
    if (err_) {
        out = *err_;
        return true;
    }
    if (writing_ || buf_.is_empty())
        return false;
    size_t n = buf_.size() < into.size() ? buf_.size() : into.size();
    memcpy(into.data(), buf_.held().data(), n);
    buf_.consume(n);
    out = n;
    return true;
}

bool File::put_fast(u32 c, result<void> &out)
{
    if (err_) {
        out = *err_;
        return true;
    }
    if (!writing_ || !buf_.ready() || buffers_nothing())
        return false;
    if (how_ == Buffering::Line && c == '\n')
        return false;
    if (buf_.append_rune(c) == 0)
        return false;
    out = ok();
    return true;
}

bool File::write_fast(Span<u8> s, result<void> &out)
{
    if (err_) {
        out = *err_;
        return true;
    }
    if (!writing_ || !buf_.ready() || buffers_nothing())
        return false;
    if (s.size() > buf_.room())
        return false;
    if (how_ == Buffering::Line)
        for (u8 b : s)
            if (b == '\n')
                return false;
    buf_.append(s);
    out = ok();
    return true;
}

/// A whole line in hand, or nothing taken: `take_line` consumes the fragment
/// it could not finish, which the slow half would then have to be told about.
bool File::line_fast(String &line, bool keep_nl, result<bool> &out)
{
    if (err_) {
        out = eof() ? result<bool>(false) : result<bool>(*err_);
        return true;
    }
    if (writing_ || !buf_.has_line())
        return false;
    line.clear();
    buf_.take_line(line, keep_nl);
    out = true;
    return true;
}

// ---------------------------------------------------------------------------
// The wire
// ---------------------------------------------------------------------------

task<result<size_t>> File::fill()
{
    // The tie: a prompt is out before what answers it is read.
    if (tie_out_)
        co_await out().flush();

    if (src_) {
        // What a rune straddling two chunks left behind: three bytes at most,
        // which is why `Input::read` hands back bytes.
        String carry(reinterpret_cast<const char *>(buf_.held().data()), buf_.size());
        if (carry.size() > 4)
            co_return mk_err(EINVAL);
        CO_LET(chunk, co_await src_->read());
        size_t had   = carry.size();
        carry += chunk;
        if (carry.size() <= had)
            co_return Error::closed();
        buf_.adopt(std::move(carry));
        co_return buf_.size() - had;
    }

    block_ready();
    buf_.compact();
    if (buf_.room() == 0)
        co_return mk_err(EINVAL);

    u32 want = u32(buf_.room());
    String got;
    CO_LET(n, co_await detail::read_into(fd_, got, want));
    if (n == 0)
        co_return Error::closed();
    if (n > buf_.room())
        n = buf_.room();
    memcpy(buf_.tail().data(), got.data(), n);
    buf_.filled(n);
    co_return n;
}

task<result<void>> File::drain()
{
    while (!buf_.is_empty()) {
        String held(reinterpret_cast<const char *>(buf_.held().data()), buf_.size());
        CO_LET(n, co_await detail::write_once(fd_, bytes_of(held)));
        if (n == 0)
            co_return mk_err(EIO);
        buf_.consume(n);
    }
    co_return ok();
}

task<result<void>> File::flush()
{
    if (!writing_ || buf_.is_empty())
        co_return ok();
    result<void> r = co_await drain();
    if (!r.ok())
        co_return fail_with(r.error());
    co_return ok();
}

/// `Auto` decides here, once: a character device is the console.
task<void> File::probe()
{
    how_ = (co_await detail::is_console(fd_)) ? Buffering::Line : Buffering::Full;
}

task<result<void>> File::settle(bool to_write)
{
    if (how_ == Buffering::Auto)
        co_await probe();
    if (writing_ == to_write)
        co_return ok();
    if (writing_) {
        CO_OK(co_await flush());
        writing_ = false;
        co_return ok();
    }

    // Read to write: the read-ahead goes back to the descriptor.
    if (!buf_.is_empty()) {
        if (src_)
            co_return mk_err(EOPNOTSUPP);
        i64 back = -i64(buf_.size());
        if (!(co_await seek_fd(fd_, back, SEEK_CUR)).ok())
            co_return mk_err(EOPNOTSUPP);
        buf_.reset();
    }
    writing_ = true;
    co_return ok();
}

// ---------------------------------------------------------------------------
// Input
// ---------------------------------------------------------------------------

task<result<u32>> File::get()
{
    result<u32> fast = u32(0);
    if (take_fast(fast))
        co_return std::move(fast);
    if (!readable())
        co_return fail_with(mk_err(EINVAL));
    if (writing_) {
        result<void> s = co_await settle(false);
        if (!s.ok())
            co_return fail_with(s.error());
    }

    for (;;) {
        u32 c = 0;
        if (buf_.ready() && buf_.take(c) == RuneStep::Got)
            co_return c;
        result<size_t> got = co_await fill();
        if (!got.ok()) {
            // A sequence end of input cut short.
            if (got.error() == Kind::Closed && buf_.ready() && !buf_.is_empty())
                co_return buf_.take_broken();
            co_return fail_with(got.error());
        }
    }
}

task<result<size_t>> File::read(SpanMut<u8> into)
{
    result<size_t> fast = size_t(0);
    if (read_fast(into, fast))
        co_return std::move(fast);
    if (!readable())
        co_return fail_with(mk_err(EINVAL));
    if (writing_) {
        result<void> s = co_await settle(false);
        if (!s.ok())
            co_return fail_with(s.error());
    }
    if (into.empty())
        co_return size_t(0);

    // Nothing in hand, and a span bigger than the buffer: read into it.
    if (buf_.is_empty() && !src_ && (how_ == Buffering::None || into.size() >= want_)) {
        String got;
        u32 want = u32(into.size() < size_t(READ_MAX) ? into.size() : size_t(READ_MAX));
        result<size_t> n = co_await detail::read_into(fd_, got, want);
        if (!n.ok())
            co_return fail_with(n.error());
        if (n.value() == 0)
            co_return fail_with(Error::closed());
        size_t k = n.value() < into.size() ? n.value() : into.size();
        memcpy(into.data(), got.data(), k);
        co_return k;
    }

    if (buf_.is_empty()) {
        result<size_t> got = co_await fill();
        if (!got.ok())
            co_return fail_with(got.error());
    }
    size_t n = buf_.size() < into.size() ? buf_.size() : into.size();
    memcpy(into.data(), buf_.held().data(), n);
    buf_.consume(n);
    co_return n;
}

task<result<bool>> File::getline(String &out, bool keep_nl)
{
    result<bool> fast = false;
    if (line_fast(out, keep_nl, fast))
        co_return std::move(fast);
    out.clear();
    if (!readable())
        co_return fail_with(mk_err(EINVAL));
    if (writing_) {
        result<void> s = co_await settle(false);
        if (!s.ok())
            co_return fail_with(s.error());
    }

    size_t seen = 0;
    for (;;) {
        if (buf_.ready() && !buf_.is_empty()) {
            size_t was = buf_.size();
            LineStep step = buf_.take_line(out, keep_nl);
            seen += was - buf_.size();
            if (step == LineStep::Done)
                co_return true;
        }
        result<size_t> got = co_await fill();
        if (!got.ok()) {
            // A final fragment with no newline is a line.
            if (got.error() == Kind::Closed) {
                fail_with(Error::closed());
                co_return seen != 0;
            }
            co_return fail_with(got.error());
        }
    }
}

// ---------------------------------------------------------------------------
// Output
// ---------------------------------------------------------------------------

task<result<void>> File::put(u32 c)
{
    result<void> fast = ok();
    if (put_fast(c, fast))
        co_return std::move(fast);
    if (!writable())
        co_return fail_with(mk_err(EINVAL));
    {
        result<void> s = co_await settle(true);
        if (!s.ok())
            co_return fail_with(s.error());
    }

    u8 e[4];
    size_t n = utf8_encode(c, e);
    if (how_ == Buffering::None) {
        result<void> w = co_await detail::write_bytes(fd_, Span<u8>(e, n));
        if (!w.ok())
            co_return fail_with(w.error());
        co_return ok();
    }

    block_ready();
    if (buf_.append_rune(c) == 0) {
        CO_OK(co_await flush());
        if (buf_.append_rune(c) == 0)
            co_return fail_with(mk_err(EINVAL));
    }
    if (how_ == Buffering::Line && c == '\n')
        CO_OK(co_await flush());
    co_return ok();
}

task<result<void>> File::write(Str s)
{
    co_return co_await write_bytes(bytes_of(s));
}

task<result<void>> File::write_bytes(Span<u8> s)
{
    result<void> fast = ok();
    if (write_fast(s, fast))
        co_return std::move(fast);
    if (!writable())
        co_return fail_with(mk_err(EINVAL));
    {
        result<void> st = co_await settle(true);
        if (!st.ok())
            co_return fail_with(st.error());
    }

    // Longer than the buffer: through it rather than into it.
    if (how_ == Buffering::None || s.size() >= want_) {
        CO_OK(co_await flush());
        result<void> w = co_await detail::write_bytes(fd_, s);
        if (!w.ok())
            co_return fail_with(w.error());
        co_return ok();
    }

    block_ready();
    if (s.size() > buf_.room())
        CO_OK(co_await flush());
    buf_.append(s);
    if (how_ == Buffering::Line)
        for (u8 b : s)
            if (b == '\n') {
                CO_OK(co_await flush());
                break;
            }
    co_return ok();
}

// ---------------------------------------------------------------------------
// The rest
// ---------------------------------------------------------------------------

task<result<u64>> File::seek(i64 off, u32 whence)
{
    if (writing_)
        CO_OK(co_await flush());
    else if (whence == SEEK_CUR)
        // The descriptor is as far ahead as the read-ahead in hand.
        off -= i64(buf_.size());
    buf_.reset();
    writing_ = false;

    result<u64> at = co_await seek_fd(fd_, off, whence);
    if (!at.ok())
        co_return fail_with(at.error());
    if (eof())
        clear_err();
    co_return at.value();
}

task<result<void>> File::close()
{
    if (closed_)
        co_return ok();
    closed_ = true;

    result<void> res = co_await flush();
    if (own_fd_) {
        co_await close_fd(fd_);
        own_fd_ = false;
    }
    buf_.reset();
    fail_with(Error::closed());
    co_return res;
}

task<result<Handle>> File::detach()
{
    if (src_ || closed_)
        co_return mk_err(EOPNOTSUPP);
    if (writing_) {
        CO_OK(co_await flush());
    } else if (!buf_.is_empty()) {
        i64 back = -i64(buf_.size());
        if (!(co_await seek_fd(fd_, back, SEEK_CUR)).ok())
            co_return mk_err(EOPNOTSUPP);
    }
    buf_.reset();
    closed_ = true;
    own_fd_ = false;
    fail_with(Error::closed());
    co_return fd_;
}

// ---------------------------------------------------------------------------
// Scanning
// ---------------------------------------------------------------------------

task<result<u8>> File::peek()
{
    if (err_)
        co_return *err_;
    if (!readable())
        co_return fail_with(mk_err(EINVAL));
    while (!buf_.ready() || buf_.is_empty()) {
        result<size_t> got = co_await fill();
        if (!got.ok())
            co_return fail_with(got.error());
    }
    co_return buf_.held()[0];
}

task<result<void>> File::skip_space()
{
    for (;;) {
        result<u8> c = co_await peek();
        if (!c.ok())
            co_return c.error() == Kind::Closed ? ok() : result<void>(c.error());
        if (!is_space(c.value()))
            co_return ok();
        buf_.consume(1);
    }
}

task<result<bool>> File::scan_lit(u8 c)
{
    result<u8> got = co_await peek();
    if (!got.ok())
        co_return got.error() == Kind::Closed ? result<bool>(false) : result<bool>(got.error());
    if (got.value() != c)
        co_return false; // not taken, so nothing to put back
    buf_.consume(1);
    co_return true;
}

task<result<bool>> File::scan_token(String &out, size_t width)
{
    CO_OK(co_await skip_space());
    co_return co_await scan_run(out, width, [](u8 c, Str) { return !is_space(c); }, Str());
}

task<result<bool>> File::scan_until(String &out, Str stop, size_t width)
{
    co_return co_await scan_run(
        out, width, [](u8 c, Str s) { return s.find(char(c)) == Str::npos; }, stop);
}

task<result<bool>> File::scan_run(String &out, size_t width, bool (*keep)(u8, Str), Str arg)
{
    out.clear();
    for (;;) {
        if (width != 0 && out.size() >= width)
            break;
        result<u8> c = co_await peek();
        if (!c.ok()) {
            if (c.error() == Kind::Closed)
                break;
            co_return c.error();
        }
        if (!keep(c.value(), arg))
            break;
        out += char(c.value());
        buf_.consume(1);
    }
    co_return !out.empty();
}

task<result<i64>> File::scan_i64(u32 base, size_t width)
{
    CO_LET(v, co_await scan_number(base, width));
    co_return v.second ? -i64(v.first) : i64(v.first);
}

task<result<u64>> File::scan_u64(u32 base, size_t width)
{
    CO_LET(v, co_await scan_number(base, width));
    co_return v.second ? u64(0) - v.first : v.first;
}

/// The digits, one at a time. `base` 0 reads C's prefix; a `0x` with no hex
/// digit behind it is `Invalid` rather than a zero and a pushed-back `x`,
/// which is the one-pass rule above.
task<result<std::pair<u64, bool>>> File::scan_number(u32 base, size_t width)
{
    CO_OK(co_await skip_space());

    u64 v      = 0;
    size_t n   = 0;
    bool any   = false;
    bool neg   = false;
    bool have  = false;
    u8 c       = 0;

    // `peek`, with an end of input as "no byte" rather than an error. It
    // fills `have` and `c` rather than returning them, because a coroutine
    // reads better here than five copies of the same two assignments.
    auto next = [this, &have, &c]() -> task<result<void>> {
        result<u8> p = co_await peek();
        if (p.ok()) {
            have = true;
            c    = p.value();
            co_return ok();
        }
        if (p.error() == Kind::Closed) {
            have = false;
            c    = 0;
            co_return ok();
        }
        co_return p.error();
    };

    CO_OK(co_await next());

    if (have && (c == '-' || c == '+') && (width == 0 || n < width)) {
        neg = c == '-';
        buf_.consume(1);
        n++;
        CO_OK(co_await next());
    }

    if (have && c == '0' && (base == 0 || base == 16 || base == 2)) {
        buf_.consume(1);
        n++;
        any  = true;
        CO_OK(co_await next());
        if (have && (c == 'x' || c == 'X') && (base == 0 || base == 16)) {
            base = 16;
            any  = false;
            buf_.consume(1);
            n++;
            CO_OK(co_await next());
        } else if (have && (c == 'b' || c == 'B') && base == 0) {
            base = 2;
            any  = false;
            buf_.consume(1);
            n++;
            CO_OK(co_await next());
        } else if (base == 0) {
            base = 8;
        }
    } else if (base == 0) {
        base = 10;
    }

    while (have) {
        if (width != 0 && n >= width)
            break;
        u32 d = 0;
        if (!digit(c, d) || d >= base)
            break;
        v = v * base + d;
        buf_.consume(1);
        n++;
        any  = true;
        CO_OK(co_await next());
    }
    if (!any)
        co_return fail_with(mk_err(EINVAL));
    co_return std::pair<u64, bool>(v, neg);
}

// ---------------------------------------------------------------------------
// For a port that had getchar and putchar
// ---------------------------------------------------------------------------

task<result<u32>> get_rune()
{
    co_return co_await File::in().get();
}

task<result<void>> put_rune(u32 c)
{
    co_return co_await File::out().put(c);
}

task<result<void>> write_out(Str s)
{
    co_return co_await File::out().write(s);
}

task<result<void>> write_err(Str s)
{
    co_return co_await File::err().write(s);
}

} // namespace koru
