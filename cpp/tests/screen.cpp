// SPDX-License-Identifier: MIT
//
// T48's done test: T37's matrix, in C++. The real screen client on a real
// ring, against a fake daemon this test speaks itself over a `socketpair`.
//
// The fake is two threads with the other end, so it can answer *while* the
// client is parked in `ENTER` — which is the only way to get a reply out of
// order, to stop reading until the socket fills, or to go away mid-frame.
//
// **Two threads, not one.** A single thread that both read and wrote would
// block in its read while a reply waited behind it in the queue: the client
// waiting for that reply, the fake waiting for a frame, and neither moving.
//
// Needs /dev/koru, so it runs in the VM under scripts/run-cpp.sh.

#include "surface.hpp"

#include <koru/screen.hpp>

#include <cerrno>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <set>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <vector>

using namespace koru;

// ---------------------------------------------------------------------------
// The wire, as the test speaks it
// ---------------------------------------------------------------------------

namespace {

constexpr u16 OP_HELLO        = 1;
constexpr u16 OP_KEY_CLAIM    = 2;
constexpr u16 OP_KEY_READ     = 3;
constexpr u16 OP_SCREEN_CLAIM = 4;
constexpr u16 OP_BLIT         = 5;
constexpr u32 MAGIC           = 0x7263736bu;
constexpr u32 VERSION         = 1;
constexpr u16 F_STALE         = 1;
constexpr i32 THE_EINTR       = 4;

struct Frame {
    u16 op  = 0;
    u32 seq = 0;
    String body;

    u32 word(size_t i) const
    {
        const u8 *p = reinterpret_cast<const u8 *>(body.data()) + i * 4;
        return u32(p[0]) | (u32(p[1]) << 8) | (u32(p[2]) << 16) | (u32(p[3]) << 24);
    }

    size_t cells() const { return (body.size() - 40) / 8; }
};

void put32(String &out, u32 v)
{
    out += char(v & 0xff);
    out += char((v >> 8) & 0xff);
    out += char((v >> 16) & 0xff);
    out += char((v >> 24) & 0xff);
}

String words(std::initializer_list<u32> vs)
{
    String b;
    for (u32 v : vs)
        put32(b, v);
    return b;
}

String encode(u16 op, u16 flags, u32 seq, i32 res, const String &body)
{
    String f;
    put32(f, u32(16 + body.size()));
    f += char(op & 0xff);
    f += char((op >> 8) & 0xff);
    f += char(flags & 0xff);
    f += char((flags >> 8) & 0xff);
    put32(f, seq);
    put32(f, u32(res));
    f += body;
    return f;
}

/// One whole frame out of what has been read, if there is one. The length
/// checks are the fake's own: a stream that has been braided fails here, which
/// is what the single-writer case rests on.
bool take_frame(String &buf, Frame &out, String &why)
{
    if (buf.size() < 16)
        return false;
    const u8 *p = reinterpret_cast<const u8 *>(buf.data());
    size_t len  = u32(p[0]) | (u32(p[1]) << 8) | (u32(p[2]) << 16) | (u32(p[3]) << 24);
    if (len < 16 || len > 65536) {
        why = "the client sent a frame length outside the protocol";
        return false;
    }
    if (len % 8 != 0) {
        why = "a frame's length is not a multiple of eight";
        return false;
    }
    if (buf.size() < len)
        return false;
    out.op  = u16(u32(p[4]) | (u32(p[5]) << 8));
    out.seq = u32(p[8]) | (u32(p[9]) << 8) | (u32(p[10]) << 16) | (u32(p[11]) << 24);
    out.body.assign(buf.data() + 16, len - 16);
    buf.erase(0, len);
    return true;
}

// ---------------------------------------------------------------------------
// The fake daemon
// ---------------------------------------------------------------------------

class Fake {
public:
    /// A socketpair: the client's end comes back in `client`, and the fake
    /// keeps the other.
    Fake(u32 cols, u32 rows, int &client)
    {
        int sv[2] = { -1, -1 };
        if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sv) != 0) {
            FAILF("socketpair: %s", strerror(errno));
            client = -1;
            return;
        }
        mine_  = sv[0];
        client = sv[1];

        reader_ = std::thread([this, cols, rows] { read_loop(cols, rows); });
        writer_ = std::thread([this] { write_loop(); });
    }

    ~Fake()
    {
        hangup();
        if (reader_.joinable())
            reader_.join();
        if (writer_.joinable())
            writer_.join();
        if (mine_ >= 0)
            ::close(mine_);
    }

    Fake(const Fake &)            = delete;
    Fake &operator=(const Fake &) = delete;

    /// The next frame the client sent, or none if it sent none in time.
    ///
    /// **Asynchronous on purpose.** A blocking wait would hold the whole
    /// thread, and the client is single-threaded: the task that owes the frame
    /// would never be resumed and the test would deadlock against itself.
    task<Option<Frame>> take(int tries)
    {
        for (int i = 0; i < tries; i++) {
            {
                std::lock_guard<std::mutex> g(m_);
                if (!frames_.empty()) {
                    Frame f = std::move(frames_.front());
                    frames_.erase(frames_.begin());
                    co_return f;
                }
                if (done_)
                    co_return std::nullopt;
            }
            co_await sleep_for(1);
        }
        co_return std::nullopt;
    }

    void reply(const Frame &f, i32 res, u16 flags, const String &body)
    {
        send(encode(f.op, flags, f.seq, res, body));
    }

    /// Stops reading for a while, so the socket buffer fills under the client.
    void stop_reading(unsigned ms) { pause_.store(ms); }

    void hangup()
    {
        {
            std::lock_guard<std::mutex> g(m_);
            if (hungup_)
                return;
            hungup_ = true;
        }
        cv_.notify_all();
    }

    /// What the reader refused, if anything. Read after the case, because a
    /// thread cannot fail the harness.
    String complaint()
    {
        std::lock_guard<std::mutex> g(m_);
        return why_;
    }

private:
    void send(String bytes)
    {
        {
            std::lock_guard<std::mutex> g(m_);
            out_.push_back(std::move(bytes));
        }
        cv_.notify_all();
    }

    void read_loop(u32 cols, u32 rows)
    {
        String buf;
        Frame f;
        String why;

        // The handshake, which every case needs and none is about.
        for (;;) {
            if (take_frame(buf, f, why)) {
                if (f.op != OP_HELLO)
                    note("the first frame was not a HELLO");
                send(encode(OP_HELLO, 0, f.seq, 0,
                            words({ MAGIC, VERSION, cols, rows, 512, 256, 65536, 0 })));
                break;
            }
            if (!why.empty()) {
                note(why);
                return;
            }
            char chunk[8192];
            ssize_t n = ::read(mine_, chunk, sizeof(chunk));
            if (n <= 0) {
                finish();
                return;
            }
            buf.append(chunk, size_t(n));
        }

        // Chunked, and the pause is checked before **every read**, not before
        // every frame: a pause that only took effect at a frame boundary would
        // let the reader drain the socket for as long as the client kept
        // sending, and the buffer would never fill.
        for (;;) {
            unsigned ms = pause_.exchange(0);
            if (ms)
                std::this_thread::sleep_for(std::chrono::milliseconds(ms));
            char chunk[4096];
            ssize_t n = ::read(mine_, chunk, sizeof(chunk));
            if (n <= 0) {
                finish();
                return;
            }
            buf.append(chunk, size_t(n));
            while (take_frame(buf, f, why)) {
                std::lock_guard<std::mutex> g(m_);
                frames_.push_back(f);
            }
            if (!why.empty()) {
                note(why);
                finish();
                return;
            }
        }
    }

    void write_loop()
    {
        for (;;) {
            String bytes;
            {
                std::unique_lock<std::mutex> g(m_);
                cv_.wait(g, [this] { return !out_.empty() || hungup_; });
                if (out_.empty()) {
                    ::shutdown(mine_, SHUT_RDWR);
                    return;
                }
                bytes = std::move(out_.front());
                out_.erase(out_.begin());
            }
            size_t at = 0;
            while (at < bytes.size()) {
                ssize_t n = ::write(mine_, bytes.data() + at, bytes.size() - at);
                if (n <= 0)
                    return;
                at += size_t(n);
            }
        }
    }

    void note(const String &why)
    {
        std::lock_guard<std::mutex> g(m_);
        if (why_.empty())
            why_ = why;
    }

    void finish()
    {
        std::lock_guard<std::mutex> g(m_);
        done_ = true;
    }

    int mine_ = -1;
    std::thread reader_, writer_;
    std::mutex m_;
    std::condition_variable cv_;
    std::vector<Frame> frames_;
    std::vector<String> out_;
    std::atomic<unsigned> pause_{ 0 };
    bool hungup_ = false;
    bool done_   = false;
    String why_;
};

// ---------------------------------------------------------------------------
// The harness
// ---------------------------------------------------------------------------

/// Every frame the client has sent, collected by a task of its own.
///
/// This is the shape the whole suite needs: a client call sends nothing until
/// it is awaited, so a test that waits for the frame before awaiting the call
/// waits for ever. The collector runs alongside, the test awaits the call, and
/// the frames turn up here.
using Frames = std::shared_ptr<std::vector<Frame>>;
using Fakep  = std::shared_ptr<Fake>;

Task<void> collector(Fakep f, Frames into)
{
    for (;;) {
        Option<Frame> fr = co_await f->take(4000);
        if (!fr)
            co_return;
        into->push_back(*fr);
    }
}

Frames collect(Fakep f)
{
    Frames got = std::make_shared<std::vector<Frame>>();
    spawn(collector(f, got));
    return got;
}

/// The `i`-th frame the client sent, once it has sent it.
Task<Frame> frame_at(Frames got, size_t i)
{
    for (int k = 0; k < 8000; k++) {
        if (got->size() > i)
            co_return (*got)[i];
        co_await sleep_for(1);
    }
    FAILF("the client sent no frame %zu", i);
    co_return Frame{};
}

/// Answers the `i`-th frame once it arrives, from a task of its own.
Task<void> answerer(Fakep fake, Frames got, size_t i, i32 res, u16 flags, String body)
{
    Frame f = co_await frame_at(got, i);
    fake->reply(f, res, flags, body);
}

void answer(Fakep fake, Frames got, size_t i, i32 res, u16 flags, String body)
{
    spawn(answerer(fake, got, i, res, flags, std::move(body)));
}

/// Answers every blit from `at` on, so a banded flush can finish.
Task<void> blit_server(Fakep fake, Frames got, size_t at, u32 cols, u32 rows)
{
    for (size_t i = at;; i++) {
        Frame f = co_await frame_at(got, i);
        fake->reply(f, 0, 0, words({ cols, rows }));
        if (f.body.size() >= 16 && f.word(1) + f.word(3) >= rows)
            co_return;
    }
}

/// The same, with no idea how many frames there will be: eight painters mean
/// eight streams of bands, and which arrives when is the point.
Task<void> all_server(Fakep fake, Frames got, size_t at, u32 cols, u32 rows)
{
    for (size_t i = at;; i++) {
        Frame f = co_await frame_at(got, i);
        fake->reply(f, 0, 0, words({ cols, rows }));
    }
}

/// Claims the screen, which every painting case needs first. The answer comes
/// from a task of its own, because the claim is not sent until it is awaited.
Task<void> claim(Fakep fake, Frames got, Screen *screen, u32 cols, u32 rows)
{
    size_t at = got->size();
    answer(fake, got, at, 0, 0, words({ cols, rows }));
    Result<void> r = co_await screen->take_screen();
    CHECK(r.ok());
    Frame f = co_await frame_at(got, at);
    CHECK_EQ(f.op, OP_SCREEN_CLAIM);
}

/// A grid of this size filled with one letter, so a braid mixes two letters
/// inside one frame.
std::shared_ptr<Grid> painted(u32 cols, u32 rows, u32 ch)
{
    std::shared_ptr<Grid> g = std::make_shared<Grid>();
    g->resize(cols, rows);
    koru::Pane p = koru::Pane::of(*g);
    for (u32 y = 0; y < rows; y++) {
        p.move_to(0, y);
        for (u32 x = 0; x < cols; x++)
            p.put(*g, ch);
    }
    return g;
}

Task<void> painter_task(Painter p, std::shared_ptr<Grid> g, Rect whole,
                        std::shared_ptr<size_t> done)
{
    Result<bool> r = co_await p.blit(*g, whole);
    CHECK(r.ok());
    (*done)++;
}

/// Waits for `want` painters, giving the executor a turn between looks.
Task<void> await_count(std::shared_ptr<size_t> done, size_t want, int tries)
{
    for (int i = 0; i < tries && *done < want; i++)
        co_await sleep_for(1);
}

} // namespace

// ---------------------------------------------------------------------------
// The cases
// ---------------------------------------------------------------------------

namespace {

Task<void> reader_task(Keys keys, std::shared_ptr<bool> done, u32 code, u32 mods)
{
    Result<Key> k = co_await keys.next();
    CHECK(k.ok());
    if (k.ok()) {
        CHECK_EQ(k.value().code, code);
        CHECK_EQ(k.value().mods, mods);
    }
    *done = true;
}

/// Out of order: a key read parks, a blit goes out behind it, and the blit is
/// answered first. `flush` must return while the key read is still parked,
/// which is the whole reason the pump demultiplexes by `seq` rather than
/// answering whoever asked first.
Task<void> case_out_of_order(Fakep fake, Screen screen)
{
    Frames got = collect(fake);
    co_await claim(fake, got, &screen, 8, 4);

    answer(fake, got, 1, 0, 0, words({ 8, 4 }));
    CHECK((co_await screen.take_keys()).ok());
    CHECK_EQ((co_await frame_at(got, 1)).op, OP_KEY_CLAIM);

    // The key read parks: frame 2, and nothing answers it until the end.
    std::shared_ptr<bool> done = std::make_shared<bool>(false);
    spawn(reader_task(screen.keys(), done, u32('c'), MOD_CTRL));
    Frame parked = co_await frame_at(got, 2);
    CHECK_EQ(parked.op, OP_KEY_READ);

    // A blit, sent after it and answered before it.
    koru::Pane p = screen.root();
    p.write(screen.grid(), "x");
    answer(fake, got, 3, 0, 0, words({ 8, 4 }));
    CHECK((co_await screen.flush()).ok());
    CHECK_EQ((co_await frame_at(got, 3)).op, OP_BLIT);
    CHECK(!*done); // the key read is still parked

    // And only now the key.
    fake->reply(parked, 0, 0, words({ u32('c'), MOD_CTRL, 8, 4 }));
    for (int i = 0; i < 400 && !*done; i++)
        co_await sleep_for(1);
    CHECK(*done); // the parked read was answered
}

/// The single-writer rule. Several tasks paint at once, and the fake must see
/// whole frames — never two braided together.
///
/// **The writes have to be short for the braid to exist at all.** One `WRITE`
/// of a whole frame is atomic on a stream socket, so two concurrent
/// single-shot writes cannot interleave and a small blit proves nothing.
/// Eight painters against a reader that has stopped overfill the socket
/// buffer, every writer gets a short write, and the bytes of one frame are
/// then split around another's.
Task<void> case_one_writer(Fakep fake, Screen screen)
{
    constexpr size_t PAINTERS = 8;

    Frames got = collect(fake);
    co_await claim(fake, got, &screen, 512, 256);

    std::vector<std::shared_ptr<Grid>> grids;
    for (size_t i = 0; i < PAINTERS; i++)
        grids.push_back(painted(512, 256, u32('a' + i)));

    // The reader stops, so the buffer fills and the writes go short;
    // everything is answered as it is read afterwards.
    fake->stop_reading(200);
    spawn(all_server(fake, got, 1, 512, 256));

    Rect whole{ 0, 0, 512, 256 };
    std::shared_ptr<size_t> done = std::make_shared<size_t>(0);
    for (const std::shared_ptr<Grid> &g : grids)
        spawn(painter_task(screen.painter(), g, whole, done));
    co_await await_count(done, PAINTERS, 40000);
    CHECK_EQ(*done, PAINTERS);

    // Every frame is one grid's: a braid mixes two letters inside one, and the
    // fake's own length check fails before that.
    CHECK(got->size() > PAINTERS);
    std::set<u8> seen;
    for (size_t i = 1; i < got->size(); i++) {
        const Frame &f = (*got)[i];
        CHECK_EQ(f.op, OP_BLIT);
        u8 first = u8(f.body[40]);
        for (size_t c = 0; c < f.cells(); c++)
            if (u8(f.body[40 + c * 8]) != first) {
                FAILF("frame %zu carries two grids", i);
                break;
            }
        seen.insert(first);
    }
    CHECK_EQ(seen.size(), PAINTERS); // every painter's letter arrived whole
}

/// Banding: a full repaint of the largest grid there is, through a 64 KiB
/// frame. The frames must tile the damage exactly once — no cell twice, none
/// missed — and each must fit what the daemon said it would read.
Task<void> case_banding(Fakep fake, Screen screen)
{
    Frames got = collect(fake);
    co_await claim(fake, got, &screen, 512, 256);

    // Everything is damaged: the claim sized the grid. Answering is a task of
    // its own, because the flush does not return until every band has.
    spawn(blit_server(fake, got, 1, 512, 256));
    CHECK((co_await screen.flush()).ok());

    CHECK(got->size() - 1 > 1); // a full repaint of this size is many frames

    std::vector<u8> covered(512 * 256, 0);
    for (size_t i = 1; i < got->size(); i++) {
        const Frame &f = (*got)[i];
        CHECK_EQ(f.op, OP_BLIT);
        u32 x = f.word(0), y = f.word(1), w = f.word(2), h = f.word(3);
        CHECK_EQ(f.cells(), size_t(w) * h);
        CHECK(16 + 40 + f.cells() * 8 <= 65536); // a band fits one frame
        for (u32 row = y; row < y + h; row++)
            for (u32 col = x; col < x + w; col++)
                covered[size_t(row) * 512 + col]++;
    }
    for (size_t i = 0; i < covered.size(); i++)
        if (covered[i] != 1) {
            FAILF("cell %zu was sent %u times", i, covered[i]);
            break;
        }
}

/// Backpressure. The fake stops reading until the socket buffer fills; the
/// client must make progress through `POLL_ADD` rather than spinning, which is
/// asserted as a bounded `ENTER` count.
///
/// **It takes more than one painter to fill a socket.** The banding loop waits
/// for each band's reply before sending the next, so a single painter never
/// has more than one frame in flight and a 208 KiB buffer never fills.
Task<void> case_backpressure(Fakep fake, Screen screen)
{
    constexpr size_t PAINTERS   = 8;
    constexpr unsigned STOPPED  = 300;

    Frames got = collect(fake);
    co_await claim(fake, got, &screen, 512, 256);

    std::shared_ptr<Grid> g = std::make_shared<Grid>();
    g->resize(512, 256);
    Rect whole{ 0, 0, 512, 256 };

    fake->stop_reading(STOPPED);
    spawn(all_server(fake, got, 1, 512, 256));

    u64 before         = enters();
    struct timespec t0 = {}, t1 = {};
    clock_gettime(CLOCK_MONOTONIC, &t0);

    std::shared_ptr<size_t> done = std::make_shared<size_t>(0);
    for (size_t i = 0; i < PAINTERS; i++)
        spawn(painter_task(screen.painter(), g, whole, done));
    co_await await_count(done, PAINTERS, 40000);
    CHECK_EQ(*done, PAINTERS);

    clock_gettime(CLOCK_MONOTONIC, &t1);
    u64 took   = u64((t1.tv_sec - t0.tv_sec) * 1000 + (t1.tv_nsec - t0.tv_nsec) / 1000000);
    u64 spent  = enters() - before;
    u64 frames = got->size() - 1;

    // The painters cannot have finished before the reader came back, or the
    // socket never filled and this case tested nothing.
    if (took + 50 < STOPPED)
        FAILF("the painters finished in %llu ms: the socket never filled",
              (unsigned long long)took);

    // A handful of ENTERs per frame is generous: a slot, the write, the poll
    // and the reply. A writer that retried instead of polling would spend the
    // whole stopped window making syscalls.
    if (spent > frames * 12 + 4096)
        FAILF("%llu ENTERs for %llu frames in %llu ms: the writer is spinning",
              (unsigned long long)spent, (unsigned long long)frames, (unsigned long long)took);
}

/// A stale blit is not an error: the daemon resized under us, so the client
/// takes the new geometry and repaints rather than failing.
Task<void> case_stale(Fakep fake, Screen screen)
{
    Frames got = collect(fake);
    co_await claim(fake, got, &screen, 8, 4);
    koru::Pane p = screen.root();
    p.write(screen.grid(), "x");

    answer(fake, got, 1, 0, F_STALE, words({ 20, 6 }));
    CHECK((co_await screen.flush()).ok()); // a stale blit is not an error

    u32 cols = 0, rows = 0;
    screen.geometry(cols, rows);
    CHECK_EQ(cols, 20);
    CHECK_EQ(rows, 6);
    CHECK_EQ(screen.grid().cols(), 20);
    // The new grid is damaged whole, so the next flush repaints.
    CHECK_EQ(screen.grid().damage().w, 20);
}

/// A resize with no key behind it answers the parked read with `-EINTR` and a
/// payload, which is the only negative result that carries one.
Task<void> case_resize(Fakep fake, Screen screen)
{
    Frames got = collect(fake);
    co_await claim(fake, got, &screen, 8, 4);

    answer(fake, got, 1, -THE_EINTR, 0, words({ 0, 0, 20, 6 }));
    Result<Key> k = co_await screen.next_key();
    CHECK(!k.ok());
    if (!k.ok())
        CHECK(k.error() == Kind::Intr);
    CHECK_EQ((co_await frame_at(got, 1)).op, OP_KEY_READ);

    u32 cols = 0, rows = 0;
    screen.geometry(cols, rows);
    CHECK_EQ(cols, 20);
    CHECK_EQ(rows, 6);
    CHECK_EQ(screen.grid().rows(), 6); // the grid is already the new shape
}

Task<void> gone_reader(Keys keys, std::shared_ptr<bool> parked)
{
    Result<Key> k = co_await keys.next();
    CHECK(!k.ok());
    if (!k.ok())
        CHECK(k.error() == Kind::Closed);
    *parked = true;
}

/// Failure: the daemon goes away. Every parked caller is answered, every later
/// call answers without touching the wire, and nothing hangs — which is why
/// the alarm is armed and stdout is line-buffered.
Task<void> case_gone(Fakep fake, Screen screen)
{
    Frames got = collect(fake);
    co_await claim(fake, got, &screen, 8, 4);

    std::shared_ptr<bool> parked = std::make_shared<bool>(false);
    spawn(gone_reader(screen.keys(), parked));
    CHECK_EQ((co_await frame_at(got, 1)).op, OP_KEY_READ); // never answered

    fake->hangup();
    for (int i = 0; i < 1000 && !*parked; i++)
        co_await sleep_for(1);
    CHECK(*parked); // the parked read was completed

    // And every later call answers from the connection, not the wire.
    koru::Pane p = screen.root();
    p.write(screen.grid(), "x");
    Result<void> f = co_await screen.flush();
    CHECK(!f.ok());
    if (!f.ok())
        CHECK(f.error() == Kind::Closed);
    Result<Key> k = co_await screen.next_key();
    CHECK(!k.ok());
    if (!k.ok())
        CHECK(k.error() == Kind::Closed);
}

using Body = Task<void> (*)(Fakep, Screen);

Task<void> with_screen(Fakep fake, int fd, Body body)
{
    Result<Screen> s = co_await Screen::own(fd);
    CO_REQUIRE(s.ok());
    co_await body(fake, std::move(s).take());
}

/// One case: the fake, the ring, the handshake, and then the body.
void screen_case(u32 cols, u32 rows, Body body)
{
    int client = -1;
    Fakep fake = std::make_shared<Fake>(cols, rows, client);
    if (client < 0)
        return;
    // koru admits a non-regular file only when it was opened non-blocking.
    int flags = fcntl(client, F_GETFL);
    CHECK(flags >= 0 && fcntl(client, F_SETFL, flags | O_NONBLOCK) == 0);

    run(with_screen(fake, client, body));

    String why = fake->complaint();
    if (!why.empty())
        FAILF("the fake daemon refused a frame: %s", why.c_str());
}

} // namespace

CASE(screen_a_reply_out_of_order_wakes_the_caller_that_asked_for_it)
{
    screen_case(8, 4, case_out_of_order);
}

CASE(screen_several_blits_at_once_are_whole_frames)
{
    screen_case(512, 256, case_one_writer);
}

CASE(screen_a_full_repaint_bands_into_frames_that_tile_the_damage)
{
    screen_case(512, 256, case_banding);
}

CASE(screen_a_full_socket_parks_the_writer_rather_than_spinning)
{
    screen_case(512, 256, case_backpressure);
}

CASE(screen_a_stale_blit_resizes_the_grid_instead_of_failing)
{
    screen_case(8, 4, case_stale);
}

CASE(screen_a_resize_answers_a_parked_key_read_with_the_new_geometry)
{
    screen_case(8, 4, case_resize);
}

CASE(screen_a_daemon_that_goes_away_completes_everyone_and_never_hangs)
{
    screen_case(8, 4, case_gone);
}
