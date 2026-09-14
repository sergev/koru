// SPDX-License-Identifier: MIT
//
// Braam's `src/cmd/less.cpp`, unchanged but for the include line: a pager on a
// terminal, a `cat` when stdout is not one.
//
// It names no socket, no daemon, no protocol and no window. It claims the
// keys, claims the screen, paints panes and reads keys — the same six methods
// Braam's pager calls, over a transport that did not exist when it was
// written.
//
// `rust/runtime/examples/less.rs` is the same program in the other binding,
// and scripts/e2e.sh runs both against the daemon and compares the two window
// snapshots **byte for byte**. That is T48's done test, and it is a stronger
// claim than two demos printing the same text: it is made on a surface that
// paints.

#include <koru/braam.hpp>

#include <cstdio>

namespace {

const char *USAGE = "Usage:\n"
                    "    less [<file>]\n";

struct Pager {
    TextBuf buf;
    TextView view;
    String name;
};

void paint(Pager &p, Screen &screen)
{
    Pane body = screen.body();
    u32 h     = body.height();
    p.view.paint(body, screen.grid(), p.buf);

    size_t lines = p.buf.lines();
    size_t shown = lines < p.view.top() + h ? lines : p.view.top() + h;
    size_t pct   = lines > 0 ? shown * 100 / lines : 100;
    const char *name = p.name.empty() ? "stdin" : p.name.c_str();

    char line[256];
    snprintf(line, sizeof(line), " %s  %zu%%  %s\xe2\x80\x94 q quits, arrows and PgUp/PgDn scroll",
             name, pct, shown == lines ? "END  " : "");

    Pane bar = screen.status();
    bar.style(KS_COLOR_BLACK, KS_COLOR_CYAN, 0);
    bar.move_to(0, 0);
    bar.write(screen.grid(), line);
    bar.fill_row(screen.grid());
}

/// `cat.cpp`'s loop: chunks, so a last line without a newline stays one.
Task<i32> cat(Args args)
{
    Input files(args.tail(), in_fd(), "less");
    for (;;) {
        Result<String> chunk = co_await files.read();
        if (!chunk.ok()) {
            if (chunk.error() == Kind::Closed)
                co_return 0;
            co_return chunk.error() == Kind::Cancelled ? 130 : 1;
        }
        if (!(co_await write_all(out_fd(), chunk.value())).ok())
            co_return 1;
    }
}

} // namespace

Task<Result<i32>> koru_main(Args args)
{
    if (help_asked(args))
        co_return co_await usage_asked(USAGE);
    if (args.size() > 2)
        co_return co_await usage_error(USAGE);

    // No terminal is not an error: it is `cat`. The screen connection is the
    // whole of the question — with no daemon and none to start, there is
    // nothing to paint on.
    Result<Screen> got = co_await Screen::connect();
    if (!got.ok())
        co_return co_await cat(args);
    Screen screen = std::move(got).take();

    // The keys before the input is read, so that anything typed while a slow
    // pipe fills is queued for us rather than echoed at the shell.
    if (!(co_await screen.take_keys()).ok()) {
        co_await write_all(err_fd(), "less: no keyboard\n");
        co_return 1;
    }

    Pager p;
    if (args.size() == 2)
        p.name = String(args[1]);

    LineReader lines{ Input(args.tail(), in_fd(), "less") };
    String line;
    for (;;) {
        Result<bool> more = co_await lines.next(line);
        if (!more.ok())
            co_return more.error() == Kind::Cancelled ? 130 : 1;
        if (!more.value())
            break;
        p.buf.add(line);
    }

    if (!(co_await screen.take_screen()).ok()) {
        co_await write_all(err_fd(), "less: no screen\n");
        co_return 1;
    }

    for (;;) {
        paint(p, screen);
        if (!(co_await screen.flush()).ok())
            co_return 1;

        Result<Key> got_key = co_await screen.next_key();
        if (!got_key.ok()) {
            // Intr is a resize with no key behind it: the grid is already the
            // new shape, so the top of the loop repaints it.
            if (got_key.error() == Kind::Intr || got_key.error() == Kind::Again)
                continue;
            co_return got_key.error() == Kind::Cancelled ? 130 : 1;
        }
        Key k = got_key.value();

        u32 h        = screen.body().height();
        i32 page     = h > 1 ? i32(h - 1) : 1;
        size_t count = p.buf.lines();

        u32 c = k.code;
        if (c == u32('q') || c == u32('Q') || c == KEY_ESCAPE)
            co_return 0;
        else if (c == KEY_UP || c == u32('k'))
            p.view.scroll(-1, count, h);
        else if (c == KEY_DOWN || c == u32('j') || c == KEY_ENTER)
            p.view.scroll(1, count, h);
        else if (c == KEY_PAGE_UP || c == u32('b'))
            p.view.scroll(-page, count, h);
        else if (c == KEY_PAGE_DOWN || c == u32(' ') || c == u32('f'))
            p.view.scroll(page, count, h);
        else if (c == KEY_HOME || c == u32('g'))
            p.view.to(0, count, h);
        else if (c == KEY_END || c == u32('G'))
            p.view.to(count, count, h);
    }
}
