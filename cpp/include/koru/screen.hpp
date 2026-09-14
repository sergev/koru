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

private:
    static task<result<Screen>> build(int fd, bool own);

    /// Takes up a geometry a reply reported.
    void sync();

    std::shared_ptr<Conn> conn_;
    Grid grid_;
    /// Closed by the destructor where this `Screen` opened it. -1 otherwise.
    int sock_ = -1;
};

} // namespace koru

#endif // KORU_SCREEN_HPP
