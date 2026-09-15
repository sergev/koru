// SPDX-License-Identifier: MIT
//
// Braam's `proc/screen.h`: the terminal from inside a program. A grid of its
// own, and the calls that claim the real one and blit onto it. The C++ half of
// rust/runtime/src/screen.rs.
//
// The transport is invisible above this file, which is what makes koru's
// screen a compatible replacement rather than a lookalike: `Screen` keeps its
// six methods, and what is underneath them is a Unix socket adopted into the
// ring rather than a syscall into a browser kernel.
//
// Three rules hold here, and doc/Notes.md says why each is forced:
//
//   - **The handshake is synchronous POSIX; everything after it is koru.**
//   - **The grid is ordinary heap, never an arena slot.** A slot with an op in
//     flight belongs to the kernel, so a grid living in one would be
//     unpaintable for the length of every blit. The damage is packed into a
//     slot instead, which costs one copy this project has never paid before.
//   - **Exactly one write is in flight on the socket.** `WRITE` is deferred to
//     a workqueue and koru has no op linking, so two concurrent writes on one
//     handle have no order, and on a stream socket that braids two frames into
//     permanent corruption. Senders queue behind the one in flight.
//
// **`~Screen` neither releases the claims nor closes the connection.** A
// destructor cannot await, and the koru handle `ADOPT_FD` took holds a
// reference of its own that only the ring's teardown drops. T38's EOF
// teardown is what makes that safe, and it is the argument Braam's own header
// already makes.

#ifndef KORU_SCREEN_HPP
#define KORU_SCREEN_HPP

#include <koru/grid.hpp>
#include <koru/task.hpp>
#include <koru/vocab.hpp>

#include <ks_abi.h>

#include <memory>

namespace koru {

/// A key, as a program sees it. There are no control characters: `^C` is `'c'`
/// with [`MOD_CTRL`], and the reader decides what that means.
struct Key {
    u32 code = 0;
    u32 mods = 0;

    /// A codepoint that draws, with no modifier that would make it a command.
    bool printable() const;

    friend bool operator==(Key a, Key b) { return a.code == b.code && a.mods == b.mods; }
};

// Braam's names for the protocol's key numbering, so a program says what it
// says on Braam.
inline constexpr u32 MOD_SHIFT = KS_MOD_SHIFT;
inline constexpr u32 MOD_CTRL  = KS_MOD_CTRL;
inline constexpr u32 MOD_ALT   = KS_MOD_ALT;
inline constexpr u32 MOD_META  = KS_MOD_META;

inline constexpr u32 KEY_NAMED     = KS_KEY_NAMED;
inline constexpr u32 KEY_ENTER     = KS_KEY_ENTER;
inline constexpr u32 KEY_BACKSPACE = KS_KEY_BACKSPACE;
inline constexpr u32 KEY_TAB       = KS_KEY_TAB;
inline constexpr u32 KEY_ESCAPE    = KS_KEY_ESCAPE;
inline constexpr u32 KEY_DELETE    = KS_KEY_DELETE;
inline constexpr u32 KEY_INSERT    = KS_KEY_INSERT;
inline constexpr u32 KEY_UP        = KS_KEY_UP;
inline constexpr u32 KEY_DOWN      = KS_KEY_DOWN;
inline constexpr u32 KEY_LEFT      = KS_KEY_LEFT;
inline constexpr u32 KEY_RIGHT     = KS_KEY_RIGHT;
inline constexpr u32 KEY_HOME      = KS_KEY_HOME;
inline constexpr u32 KEY_END       = KS_KEY_END;
inline constexpr u32 KEY_PAGE_UP   = KS_KEY_PAGE_UP;
inline constexpr u32 KEY_PAGE_DOWN = KS_KEY_PAGE_DOWN;

// The palette and the attributes, in Braam's spelling of the protocol's.
inline constexpr u8 ATTR_BOLD      = KS_ATTR_BOLD;
inline constexpr u8 ATTR_UNDERLINE = KS_ATTR_UNDERLINE;
inline constexpr u8 ATTR_REVERSE   = KS_ATTR_REVERSE;

inline constexpr u8 COLOR_BLACK   = KS_COLOR_BLACK;
inline constexpr u8 COLOR_RED     = KS_COLOR_RED;
inline constexpr u8 COLOR_GREEN   = KS_COLOR_GREEN;
inline constexpr u8 COLOR_YELLOW  = KS_COLOR_YELLOW;
inline constexpr u8 COLOR_BLUE    = KS_COLOR_BLUE;
inline constexpr u8 COLOR_MAGENTA = KS_COLOR_MAGENTA;
inline constexpr u8 COLOR_CYAN    = KS_COLOR_CYAN;
inline constexpr u8 COLOR_WHITE   = KS_COLOR_WHITE;
inline constexpr u8 COLOR_BRIGHT  = KS_COLOR_BRIGHT;

/// One blit frame's payload: the header, then the cells row by row. Pure, so
/// it is tested without a socket.
String pack_blit(const Grid &g, Rect d);

/// Where the daemon's socket is. `$KORU_SCREEN_SOCK` names one outright;
/// otherwise it is the protocol's own name under `$XDG_RUNTIME_DIR`.
String sock_path();

class Conn;

/// The key half of a screen, on its own so that a parked read and a blit can
/// be in flight at once. Cheap and copyable: it holds the connection alone.
class Keys {
public:
    explicit Keys(std::shared_ptr<Conn> c) : conn_(std::move(c)) {}

    /// The next key. `Intr` is a resize that arrived with no key behind it;
    /// the geometry is already recorded, so the caller repaints and asks again.
    task<result<Key>> next();

private:
    std::shared_ptr<Conn> conn_;
};

/// The painting half, on its own for the reason [`Keys`] is: two tasks may
/// paint at once, and the connection serialises them.
class Painter {
public:
    explicit Painter(std::shared_ptr<Conn> c) : conn_(std::move(c)) {}

    /// Sends `d` of `g` as blit frames, banded to what one frame holds.
    /// `false` is a stale blit: the daemon has resized, nothing was drawn, and
    /// its reply carried the new geometry.
    task<result<bool>> blit(const Grid &g, Rect d);

private:
    std::shared_ptr<Conn> conn_;
};

/// The terminal, from inside a process: a grid of its own, and the calls that
/// claim the real one and blit onto it.
class Screen {
public:
    Screen() = default;
    ~Screen();

    Screen(const Screen &)            = delete;
    Screen &operator=(const Screen &) = delete;
    Screen(Screen &&o) noexcept;
    Screen &operator=(Screen &&o) noexcept;

    /// Connects to the daemon, starting one if there is none, and shakes hands.
    static task<result<Screen>> connect();

    /// The same over a descriptor the caller keeps. This is what a test hands
    /// one end of a `socketpair`: `ADOPT_FD` takes a reference of its own, so
    /// the caller's descriptor stays the caller's to close.
    static task<result<Screen>> adopt(int fd);

    /// The same over a descriptor this `Screen` owns and closes.
    static task<result<Screen>> own(int fd);

    /// Binds this to a screen other than the process's own. Reserved for the
    /// multiplexing this plan defers, so the daemon answers `-ENOSYS`.
    task<result<void>> attach(u32 term_id);

    /// Claims the keyboard. Do this before reading stdin, so what is typed
    /// while a slow pipe fills is queued rather than echoed.
    task<result<void>> take_keys();

    /// Takes the alternate screen, and sizes the grid to it.
    task<result<void>> take_screen();

    Painter painter() const { return Painter(conn_); }
    Keys keys() const { return Keys(conn_); }

    Grid &grid();
    Pane root();
    /// The whole grid minus the bottom row, and that row.
    Pane body();
    Pane status();

    /// Sends the cells that changed, and the cursor with them.
    task<result<void>> flush();

    /// The next key. A resize arrives with it: the geometry rides on every
    /// reply, and the grid is resized and marked whole when it changes.
    task<result<Key>> next_key();

    /// What the daemon last said the terminal is, without asking again.
    void geometry(u32 &cols, u32 &rows) const;

    /// Whether the handshake happened. A default-constructed `Screen` paints
    /// nothing and answers `Unsupported`.
    bool connected() const { return bool(conn_); }

private:
    static task<result<Screen>> build(int fd, bool own);

    /// Takes up a geometry a reply reported.
    void sync();

    std::shared_ptr<Conn> conn_;
    Grid grid_;
    /// Closed by the destructor where this `Screen` opened it. -1 otherwise.
    int sock_ = -1;
};

// ---------------------------------------------------------------------------
// Braam's `proc/screen.h`
// ---------------------------------------------------------------------------

/// The geometry, which every reply carries so that a resize needs no event to
/// subscribe to.
struct Geometry {
    u32 cols = 0;
    u32 rows = 0;
};

/// Whether a descriptor is the terminal, and how big it is. `at` is zero
/// unless `console` is true.
struct TtyInfo {
    bool console = false;
    Geometry at;
};

/// Braam's: the only way to tell a terminal from a pipe, because the grid is
/// cells and there is no escape sequence to ask with.
///
/// **On koru the terminal is a daemon, not a descriptor**, so this asks
/// whether this process can reach a screen — which means it starts one if
/// there is none, exactly as `Screen::connect` does. That is the question a
/// pager is really asking, and answering it from `fstat` instead would make
/// every redirected `less` a `cat`. Only a standard stream can be the
/// terminal; anything else answers `console` false.
task<result<TtyInfo>> tty_of(Handle fd);

namespace detail {

/// This process's screen, whether or not it is connected yet.
Screen &proc_screen();

/// Connects it, once. `Unsupported` where there is no screen to be had, and
/// the same answer to every later ask: a daemon that could not be started once
/// will not be started per call.
task<result<void>> proc_connect();

/// Forget it. A new ring means a new connection, so `install` and `shutdown`
/// call this as they call `reset_std`.
void reset_screen();

} // namespace detail

/// Braam's `ProcScreen`: the terminal from inside a process, default-
/// constructed and claimed lazily.
///
/// It holds nothing. The connection is the *process's*, not this object's,
/// which is what lets `tty_of` and a `ProcScreen` made after it share one —
/// and it is what Braam's own header means by "this process's own terminal".
class ProcScreen {
public:
    ProcScreen()  = default;
    ~ProcScreen() = default;

    ProcScreen(const ProcScreen &)            = delete;
    ProcScreen &operator=(const ProcScreen &) = delete;

    /// Binds to a screen other than this process's own. Reserved for the
    /// multiplexing this plan defers, so the daemon answers `-ENOSYS`.
    task<result<void>> attach(u32 term_id);

    task<result<void>> take_keys();
    task<result<void>> take_screen();

    Grid &grid();
    GridPane root();
    /// The whole grid minus the bottom row, and that row.
    GridPane body();
    GridPane status();

    task<result<void>> flush();
    task<result<Key>> next_key();
};

} // namespace koru

#endif // KORU_SCREEN_HPP
