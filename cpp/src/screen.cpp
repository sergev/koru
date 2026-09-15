// SPDX-License-Identifier: MIT

#include <koru/screen.hpp>

#include <koru/ops.hpp>
#include <koru/rt.hpp>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>
#include <unordered_map>
#include <vector>

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

u32 le32(const u8 *p)
{
    return u32(p[0]) | (u32(p[1]) << 8) | (u32(p[2]) << 16) | (u32(p[3]) << 24);
}

void put32(String &out, u32 v)
{
    out += char(v & 0xff);
    out += char((v >> 8) & 0xff);
    out += char((v >> 16) & 0xff);
    out += char((v >> 24) & 0xff);
}

void put16(String &out, u16 v)
{
    out += char(v & 0xff);
    out += char((v >> 8) & 0xff);
}

void nap_ms(unsigned ms)
{
    struct timespec t = { long(ms / 1000), long(ms % 1000) * 1000000 };
    nanosleep(&t, nullptr);
}

} // namespace

// ---------------------------------------------------------------------------
// The connection
// ---------------------------------------------------------------------------

/// One reply: the header, and whatever payload came with it.
struct Reply {
    u16 flags = 0;
    i32 res   = 0;
    String body;

    /// The payload's first `n` bytes, or nullptr where it is shorter.
    const u8 *payload(size_t n) const
    {
        return body.size() >= n ? reinterpret_cast<const u8 *>(body.data()) : nullptr;
    }
};

class Conn {
public:
    explicit Conn(Handle ctrl) : ctrl_(ctrl) {}

    Handle ctrl() const { return ctrl_; }

    u32 seq()
    {
        // Never 0: that is the protocol's reserved value for an unsolicited
        // frame, and a request carrying it is refused.
        next_seq_++;
        if (next_seq_ == 0)
            next_seq_ = 1;
        return next_seq_;
    }

    /// Files a reply and wakes whoever is waiting for it.
    void deliver(u32 seq, Reply r)
    {
        replies_[seq] = std::move(r);
        auto it       = waiters_.find(seq);
        if (it != waiters_.end()) {
            std::coroutine_handle<> h = it->second;
            waiters_.erase(it);
            reactor().defer(h);
        }
    }

    bool has_reply(u32 seq) const { return replies_.count(seq) != 0; }

    Reply take_reply(u32 seq)
    {
        auto it  = replies_.find(seq);
        Reply out = std::move(it->second);
        replies_.erase(it);
        return out;
    }

    void park(u32 seq, std::coroutine_handle<> h) { waiters_[seq] = h; }

    /// The far end has gone. Everyone parked is answered, and every later call
    /// is answered without a syscall.
    void kill(Error e)
    {
        if (!dead_)
            dead_ = e;
        std::vector<std::coroutine_handle<>> woken;
        for (const auto &kv : waiters_)
            woken.push_back(kv.second);
        waiters_.clear();
        for (std::coroutine_handle<> h : senders_)
            woken.push_back(h);
        senders_.clear();
        for (std::coroutine_handle<> h : woken)
            reactor().defer(h);
    }

    Option<Error> error() const { return dead_; }

    // The write token. A sender that finds one in flight parks; the one that
    // finishes hands it to the next. Nothing here is a lock: the runtime is
    // single threaded, and this is an ordering rule, not a mutual exclusion
    // one.
    bool take_write()
    {
        if (dead_ || !writing_) {
            writing_ = true;
            return true;
        }
        return false;
    }

    void queue_writer(std::coroutine_handle<> h) { senders_.push_back(h); }

    void release_write()
    {
        if (senders_.empty()) {
            writing_ = false;
            return;
        }
        std::coroutine_handle<> h = senders_.front();
        senders_.erase(senders_.begin());
        // The token is handed over rather than released: the woken sender owns
        // it the moment it runs.
        reactor().defer(h);
    }

    u32 cols = 0, rows = 0;
    u32 max_frame = KS_MAX_FRAME;

private:
    Handle ctrl_;
    std::unordered_map<u32, Reply> replies_;
    std::unordered_map<u32, std::coroutine_handle<>> waiters_;
    /// Whoever is waiting for the socket to be writable, in arrival order.
    std::vector<std::coroutine_handle<>> senders_;
    bool writing_ = false;
    u32 next_seq_ = 0;
    /// Sticky: once the far end has gone, every call answers with it and
    /// nothing touches the wire again.
    Option<Error> dead_;
};

namespace {

/// Waits for the reply carrying `seq`.
struct ReplyAwaiter {
    Conn *conn;
    u32 seq;

    bool await_ready() const { return conn->has_reply(seq) || conn->error().has_value(); }
    void await_suspend(std::coroutine_handle<> h) const { conn->park(seq, h); }

    result<Reply> await_resume() const
    {
        if (conn->has_reply(seq))
            return conn->take_reply(seq);
        return *conn->error();
    }
};

/// Waits for the one write in flight to finish.
struct WriteToken {
    Conn *conn;

    bool await_ready() const { return conn->take_write(); }
    void await_suspend(std::coroutine_handle<> h) const { conn->queue_writer(h); }
    void await_resume() const {}
};

/// The pump: the one task that reads the socket. Everything else waits for a
/// `seq` it filed, which is what keeps a reply that arrives out of order from
/// waking the wrong caller.
task<void> pump(std::shared_ptr<Conn> conn)
{
    String buf;
    for (;;) {
        String got;
        result<size_t> n = co_await detail::read_into(conn->ctrl(), got, 0);
        if (!n.ok()) {
            conn->kill(n.error());
            co_return;
        }
        if (n.value() == 0) {
            conn->kill(Error::closed());
            co_return;
        }
        buf += got;

        // One read can carry three whole replies or half of one: this is the
        // place the plan says a second implementer conflates `seq` with a koru
        // cookie, and the frames are why they cannot be the same number.
        size_t at = 0;
        while (buf.size() - at >= 16) {
            const u8 *p = reinterpret_cast<const u8 *>(buf.data()) + at;
            size_t len  = le32(p);
            if (len < 16 || len > KS_MAX_FRAME) {
                conn->kill(mk_err(EPROTO));
                co_return;
            }
            if (buf.size() - at < len)
                break;
            Reply r;
            r.flags = u16(u32(p[6]) | (u32(p[7]) << 8));
            r.res   = i32(le32(p + 12));
            r.body.assign(buf.data() + at + 16, len - 16);
            conn->deliver(le32(p + 8), std::move(r));
            at += len;
        }
        buf.erase(0, at);
    }
}

/// Sends one whole frame, with exactly one write in flight on the socket.
task<result<void>> send(Conn *conn, String frame)
{
    if (Option<Error> e = conn->error())
        co_return *e;

    co_await WriteToken{ conn };

    result<void> out = ok();
    if (Option<Error> e = conn->error())
        out = *e;
    else
        out = co_await detail::write_bytes(
            conn->ctrl(), Span<u8>(reinterpret_cast<const u8 *>(frame.data()), frame.size()));

    conn->release_write();
    if (!out.ok()) {
        conn->kill(out.error());
        co_return out.error();
    }
    co_return ok();
}

/// Sends a request and waits for the reply that carries its `seq`, whatever
/// its result is. The caller maps it: `-EINTR` on a key read carries a payload
/// and every other error does not, so one of them must see the raw reply.
task<result<Reply>> request_raw(Conn *conn, u32 op, u16 flags, String payload)
{
    u32 seq = conn->seq();
    String frame;
    put32(frame, u32(16 + payload.size()));
    put16(frame, u16(op));
    put16(frame, flags);
    put32(frame, seq);
    put32(frame, 0);
    frame += payload;
    CO_OK(co_await send(conn, std::move(frame)));

    co_return co_await ReplyAwaiter{ conn, seq };
}

/// The same, with a negative result turned into an error. This protocol's
/// errnos are koru's errnos: one table, so a program sees the same names on
/// either side of the socket.
task<result<Reply>> request(Conn *conn, u32 op, u16 flags, String payload)
{
    CO_LET(r, co_await request_raw(conn, op, flags, std::move(payload)));
    if (r.res < 0)
        co_return mk_err(-r.res);
    co_return std::move(r);
}

/// How many rows of a band this many cells wide fit one frame. At least one,
/// or the geometry could not be sent at all — which is what `KS_MIN_SLOT`
/// exists to guarantee.
result<u32> rows_per_band(u32 max_frame, u32 w)
{
    size_t room = size_t(max_frame) - 16 - 40;
    size_t row  = size_t(w) * 8;
    if (row == 0 || row > room)
        return mk_err(EINVAL);
    return u32(room / row);
}

} // namespace

// ---------------------------------------------------------------------------
// The pure halves
// ---------------------------------------------------------------------------

bool Key::printable() const
{
    return code >= 0x20 && code != 0x7f && code < KEY_NAMED &&
           (mods & (MOD_CTRL | MOD_ALT | MOD_META)) == 0;
}

String pack_blit(const Grid &g, Rect d)
{
    String out;
    out.reserve(40 + size_t(d.w) * d.h * 8);
    const u32 head[10] = { d.x,         d.y,         d.w,   d.h,       g.cursor_x,
                           g.cursor_y,  u32(g.cursor_on), g.cols(), g.rows(), 0 };
    for (u32 v : head)
        put32(out, v);

    for (u32 y = d.y; y < d.y + d.h; y++)
        for (u32 x = d.x; x < d.x + d.w; x++) {
            const ks_cell *c = g.at(x, y);
            ks_cell blank    = {};
            if (!c)
                c = &blank;
            put32(out, c->ch);
            out += char(c->fg);
            out += char(c->bg);
            out += char(c->attrs);
            out += char(0);
        }
    return out;
}

// ---------------------------------------------------------------------------
// Keys and Painter
// ---------------------------------------------------------------------------

task<result<Key>> Keys::next()
{
    CO_LET(r, co_await request_raw(conn_.get(), KS_OP_KEY_READ, 0, String()));
    const u8 *b = r.payload(16);
    if (!b) {
        // -EINTR is the only negative result that carries a payload, and this
        // is why: a resize has to be answered, not signalled, so the new
        // geometry rides on the refusal itself.
        co_return r.res < 0 ? mk_err(-r.res) : mk_err(EINVAL);
    }
    if (le32(b + 8) != 0 && le32(b + 12) != 0) {
        conn_->cols = le32(b + 8);
        conn_->rows = le32(b + 12);
    }
    if (r.res < 0)
        co_return mk_err(-r.res);
    co_return Key{ le32(b), le32(b + 4) };
}

task<result<bool>> Painter::blit(const Grid &g, Rect d)
{
    if (d.w == 0 || d.h == 0)
        co_return true;
    u32 rows = CO_TRY(rows_per_band(conn_->max_frame, d.w));

    u32 y = d.y;
    while (y < d.y + d.h) {
        u32 h = rows < d.y + d.h - y ? rows : d.y + d.h - y;
        Rect band{ d.x, y, d.w, h };
        CO_LET(r, co_await request(conn_.get(), KS_OP_BLIT, 0, pack_blit(g, band)));
        if (r.flags & KS_F_STALE) {
            const u8 *b = r.payload(8);
            if (!b)
                co_return mk_err(EINVAL);
            conn_->cols = le32(b);
            conn_->rows = le32(b + 4);
            co_return false;
        }
        y += h;
    }
    co_return true;
}

// ---------------------------------------------------------------------------
// The socket
// ---------------------------------------------------------------------------

namespace {

/// koru admits a non-regular file only when it was opened non-blocking, and it
/// never sets that bit on a descriptor it did not open.
bool make_nonblocking(int fd)
{
    int flags = fcntl(fd, F_GETFL);
    return flags >= 0 && fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
}

/// One connect attempt. -1 with `refused` set where a listener answered
/// `ECONNREFUSED`, which is not proof of a dead daemon.
int connect_once(const String &path, bool &refused)
{
    refused = false;
    struct sockaddr_un addr = {};
    if (path.size() + 1 > sizeof(addr.sun_path))
        return -1;
    addr.sun_family = AF_UNIX;
    memcpy(addr.sun_path, path.c_str(), path.size());

    int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0)
        return -1;
    if (::connect(fd, reinterpret_cast<struct sockaddr *>(&addr), sizeof(addr)) == 0)
        return fd;
    refused = errno == ECONNREFUSED;
    ::close(fd);
    return -1;
}

/// One connect, retried briefly on a refusal.
///
/// **A refusal is not proof of a dead daemon.** A listening socket whose accept
/// queue is full answers `ECONNREFUSED` too, and twenty clients starting at
/// once are exactly that case: without this, some of them would decide a live
/// daemon's socket was stale.
int try_connect(const String &path)
{
    for (int i = 0; i < 40; i++) {
        bool refused = false;
        int fd       = connect_once(path, refused);
        if (fd >= 0)
            return fd;
        if (!refused)
            return -1; // no socket at all: nothing to wait for
        nap_ms(5);
    }
    return -1;
}

/// The lock file beside the socket. Its directory is checked here: a runtime
/// directory that is not ours, or that anyone may write to, is not somewhere
/// to put a socket other programs will trust.
result<int> lock_file(const String &path)
{
    size_t slash = path.rfind('/');
    if (slash == String::npos)
        return mk_err(EINVAL);
    String dir = slash == 0 ? String("/") : path.substr(0, slash);

    struct stat st = {};
    if (::stat(dir.c_str(), &st) != 0)
        return Error::from_errno(Errno::last());
    if (!S_ISDIR(st.st_mode))
        return mk_err(ENOTDIR);
    // `/tmp` is the fallback and is 1777, so the check is on the socket's
    // owner rather than the directory's mode there; under $XDG_RUNTIME_DIR it
    // is both.
    if (st.st_uid != geteuid())
        return mk_err(EPERM);
    if (dir != "/tmp" && (st.st_mode & 0077) != 0)
        return mk_err(EPERM);

    int fd = ::open((path + ".lock").c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    if (fd < 0)
        return Error::from_errno(Errno::last());
    return fd;
}

/// Starts the daemon, detached: it outlives the client that started it, and a
/// window that vanished between two commands would not be a terminal.
result<void> spawn_daemon(const String &path)
{
    const char *bin = getenv("KORU_SCREEN_BIN");
    if (!bin || !*bin)
        bin = "koru-screen";

    pid_t pid = fork();
    if (pid < 0)
        return Error::from_errno(Errno::last());
    if (pid == 0) {
        setenv("KORU_SCREEN_SOCK", path.c_str(), 1);
        int null = ::open("/dev/null", O_RDWR);
        if (null >= 0) {
            dup2(null, 0);
            dup2(null, 1);
            if (null > 2)
                ::close(null);
        }
        execlp(bin, bin, nullptr);
        _exit(127);
    }
    return ok();
}

/// Connects, and starts a daemon if nothing answers.
///
/// The race is settled by an exclusive `flock` on a file beside the socket:
///
///   1. try to connect — the ordinary case, with a daemon already running;
///   2. take the lock, waiting for whoever holds it;
///   3. **try to connect again**, because the winner may have finished while
///      we waited;
///   4. only then, unlink a socket nothing is listening on and spawn.
///
/// Step 4 is what the lock is really for: **only the lock holder may unlink**.
/// Without that rule, twenty clients racing a *live* daemon would each decide
/// its socket was stale and remove it.
result<int> connect_or_spawn(const String &path)
{
    int fd = try_connect(path);
    if (fd >= 0)
        return fd;

    int lock = TRY(lock_file(path));
    // Waiting for the lock is the whole of the queue: whoever holds it is
    // either spawning or about to give up.
    for (int i = 0; i < 2000; i++) {
        if (flock(lock, LOCK_EX | LOCK_NB) == 0)
            break;
        if (errno != EWOULDBLOCK) {
            ::close(lock);
            return Error::from_errno(Errno::last());
        }
        nap_ms(5);
    }

    fd = try_connect(path);
    if (fd >= 0) { // the winner got there while we waited
        ::close(lock);
        return fd;
    }

    // Nothing is listening. A socket that is still there is stale, and this
    // process holds the lock, so it is the one allowed to say so.
    ::unlink(path.c_str());
    result<void> started = spawn_daemon(path);
    if (!started.ok()) {
        ::close(lock);
        return started.error();
    }

    // The daemon binds a temporary name and renames it into place, so a socket
    // that exists is one that answers; there is nothing to do but wait for it.
    for (int i = 0; i < 2000; i++) {
        fd = try_connect(path);
        if (fd >= 0) {
            ::close(lock);
            return fd;
        }
        nap_ms(5);
    }
    ::close(lock);
    return mk_err(ETIMEDOUT);
}

} // namespace

String sock_path()
{
    const char *named = getenv("KORU_SCREEN_SOCK");
    if (named && *named)
        return String(named);
    const char *dir = getenv("XDG_RUNTIME_DIR");
    String base     = dir && *dir ? String(dir) : String("/tmp");
    return base + "/" + KS_SOCK_NAME;
}

// ---------------------------------------------------------------------------
// Screen
// ---------------------------------------------------------------------------

Screen::~Screen()
{
    // Neither releases the claims nor closes the connection: the koru handle
    // `ADOPT_FD` took holds a reference of its own, and only the ring's
    // teardown drops it. Our own descriptor goes, which is all a destructor
    // can do without awaiting.
    if (sock_ >= 0)
        ::close(sock_);
}

Screen::Screen(Screen &&o) noexcept
    : conn_(std::move(o.conn_)), grid_(std::move(o.grid_)), sock_(o.sock_)
{
    o.sock_ = -1;
}

Screen &Screen::operator=(Screen &&o) noexcept
{
    if (this != &o) {
        if (sock_ >= 0)
            ::close(sock_);
        conn_   = std::move(o.conn_);
        grid_   = std::move(o.grid_);
        sock_   = o.sock_;
        o.sock_ = -1;
    }
    return *this;
}

task<result<Screen>> Screen::connect()
{
    int fd = CO_TRY(connect_or_spawn(sock_path()));
    if (!make_nonblocking(fd)) {
        ::close(fd);
        co_return Error::from_errno(Errno::last());
    }
    co_return co_await own(fd);
}

task<result<Screen>> Screen::adopt(int fd)
{
    co_return co_await build(fd, false);
}

task<result<Screen>> Screen::own(int fd)
{
    co_return co_await build(fd, true);
}

task<result<Screen>> Screen::build(int fd, bool own)
{
    Completion c = co_await reactor().submit(sqe::adopt_fd(0, fd));
    result<u64, Errno> h = from_res(c.res);
    if (!h.ok())
        co_return as_error(h.error());
    Handle ctrl = Handle(h.value());
    register_handle(ctrl, false);

    std::shared_ptr<Conn> conn = std::make_shared<Conn>(ctrl);
    spawn(pump(conn));

    String hello;
    put32(hello, KS_MAGIC);
    put32(hello, KS_ABI_VERSION);
    result<Reply> r = co_await request(conn.get(), KS_OP_HELLO, 0, std::move(hello));
    if (!r.ok())
        co_return r.error();
    const u8 *b = r.value().payload(32);
    if (!b)
        co_return mk_err(EINVAL);
    if (le32(b) != KS_MAGIC || le32(b + 4) != KS_ABI_VERSION)
        co_return mk_err(EPROTO);
    conn->cols      = le32(b + 8);
    conn->rows      = le32(b + 12);
    u32 frame       = le32(b + 24);
    conn->max_frame = frame < KS_MAX_FRAME ? frame : KS_MAX_FRAME;

    Screen s;
    s.conn_ = std::move(conn);
    s.sock_ = own ? fd : -1;
    co_return std::move(s);
}

task<result<void>> Screen::attach(u32)
{
    CO_OK(co_await request(conn_.get(), KS_OP_TERM_OPEN, 0, String()));
    co_return ok();
}

namespace {

/// The geometry a claim reply carries, checked and taken up.
result<void> take_geometry(Conn *conn, Grid &g, const Reply &r)
{
    const u8 *b = r.payload(8);
    if (!b)
        return mk_err(EINVAL);
    u32 cols = le32(b), rows = le32(b + 4);
    if (cols == 0 || rows == 0 || cols > KS_MAX_COLS || rows > KS_MAX_ROWS)
        return mk_err(EINVAL);
    conn->cols = cols;
    conn->rows = rows;
    if (g.cols() != cols || g.rows() != rows)
        g.resize(cols, rows);
    return ok();
}

} // namespace

task<result<void>> Screen::take_keys()
{
    CO_LET(r, co_await request(conn_.get(), KS_OP_KEY_CLAIM, KS_F_TAKE, String()));
    co_return take_geometry(conn_.get(), grid_, r);
}

task<result<void>> Screen::take_screen()
{
    CO_LET(r, co_await request(conn_.get(), KS_OP_SCREEN_CLAIM, KS_F_TAKE, String()));
    co_return take_geometry(conn_.get(), grid_, r);
}

void Screen::sync()
{
    u32 cols = conn_->cols, rows = conn_->rows;
    if (cols != 0 && rows != 0 && (grid_.cols() != cols || grid_.rows() != rows))
        grid_.resize(cols, rows);
}

Grid &Screen::grid()
{
    sync();
    return grid_;
}

Pane Screen::root()
{
    sync();
    return Pane::of(grid_);
}

Pane Screen::body()
{
    Pane r = root();
    return r.height() > 1 ? r.top(r.height() - 1) : r;
}

Pane Screen::status()
{
    return root().bottom(1);
}

task<result<void>> Screen::flush()
{
    if (Option<Error> e = conn_->error())
        co_return *e;
    sync();
    Rect d      = grid_.take_damage();
    CO_LET(drawn, co_await painter().blit(grid_, d));
    if (!drawn) {
        // The daemon resized under us and its reply carried the new geometry:
        // take it up, which damages the whole grid, and let the caller flush
        // again rather than sending bands of a grid that no longer exists.
        sync();
    }
    co_return ok();
}

task<result<Key>> Screen::next_key()
{
    result<Key> got = co_await keys().next();
    sync(); // the reply's geometry, taken up before the caller paints
    co_return std::move(got);
}

void Screen::geometry(u32 &cols, u32 &rows) const
{
    cols = conn_->cols;
    rows = conn_->rows;
}

// ---------------------------------------------------------------------------
// The byte channel
// ---------------------------------------------------------------------------

namespace detail {

/// The daemon's other connection: the one whose far end is the terminal's
/// parser rather than the protocol server. It *is* stdout, so `write_all`
/// stays a plain `WRITE` with no framing in the way.
///
/// 0 unless both halves of the question say yes: this program's stdout is the
/// terminal koru was started from, and a daemon is already listening. Neither
/// is negotiable. A redirected stdout is the user's own instruction and stays
/// where it points; and **this never spawns a daemon**, because `install` runs
/// before any program has asked for a screen and a window nobody wanted is
/// worse than no window.
///
/// The handshake is synchronous POSIX, as `Screen::connect`'s preamble is:
/// there is nothing to read on this connection afterwards, so a pump task and
/// a `seq` map would be machinery for one round trip.
Handle adopt_byte_channel(Executor &ex)
{
    if (!isatty(1))
        return 0;

    bool refused = false;
    int fd       = connect_once(sock_path(), refused);
    if (fd < 0)
        return 0;

    struct timeval wait = { 2, 0 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &wait, sizeof(wait));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &wait, sizeof(wait));

    // The lengths are the protocol's own arithmetic, never a number spelled
    // here: a payload that grows must not have to be found by hand.
    String frame;
    put32(frame, ks_req_len(KS_OP_HELLO));
    put16(frame, u16(KS_OP_HELLO));
    put16(frame, KS_F_BYTES);
    put32(frame, 1); // seq, and the only one
    put32(frame, 0);
    put32(frame, KS_MAGIC);
    put32(frame, KS_ABI_VERSION);

    size_t at = 0;
    while (at < frame.size()) {
        ssize_t n = ::write(fd, frame.data() + at, frame.size() - at);
        if (n <= 0) {
            ::close(fd);
            return 0;
        }
        at += size_t(n);
    }

    // A daemon that does not know this flag answers -EINVAL, and one that does
    // not know the version closes: either way the program keeps the stdout it
    // was given.
    size_t len = ks_rep_len(KS_OP_HELLO);
    String rep(len, '\0');
    at = 0;
    while (at < len) {
        ssize_t n = ::read(fd, rep.data() + at, len - at);
        if (n <= 0) {
            ::close(fd);
            return 0;
        }
        at += size_t(n);
    }
    const u8 *p = reinterpret_cast<const u8 *>(rep.data());
    if (le32(p) != len || i32(le32(p + 12)) != 0 || le32(p + 16) != KS_MAGIC ||
        le32(p + 20) != KS_ABI_VERSION) {
        ::close(fd);
        return 0;
    }

    // koru admits a non-regular file only when it was opened non-blocking, and
    // ADOPT_FD takes a reference of its own: ours goes on the way out.
    if (!make_nonblocking(fd)) {
        ::close(fd);
        return 0;
    }
    Completion c = ex.run([](Reactor &r, int d) -> task<Completion> {
        co_return co_await r.submit(sqe::adopt_fd(0, d));
    }(ex.reactor(), fd));
    ::close(fd);
    return c.res > 0 ? Handle(c.res) : 0;
}

} // namespace detail

} // namespace koru
