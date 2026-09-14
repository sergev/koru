// SPDX-License-Identifier: MIT
//
// A window over a terminal fed from stdin: the renderer, the event pump and the
// resize path, with no protocol and no client. It is what a human looks at, and
// it is where the resize that drives screen_resize actually runs — the pixel
// oracles can synthesise an event, but not a window manager.
//
//   printf 'hello\n' | ./build/ks_show
//   KORU_SCREEN_SNAP=/tmp/x.bmp SDL_VIDEODRIVER=offscreen ./build/ks_show < file
//
// With KORU_SCREEN_SNAP set it draws one frame, writes it there and exits,
// which is how a test looks at pixels without a display.

#include "window.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {

void feed_stdin(Term &t)
{
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), stdin)) > 0)
        screen_write(t, Str(buf, n));
}

// A key back out as text, which is all this tool has to do with one. A program
// gets {code, mods} instead, over the protocol.
void report(const KsKey &k)
{
    if (k.code < KS_KEY_NAMED && k.code >= ' ' && !(k.mods & (KS_MOD_CTRL | KS_MOD_ALT)))
        return; // an ordinary character; the shell below echoes nothing
    fprintf(stderr, "key %u mods %u\n", k.code, k.mods);
}

} // namespace

int main()
{
    Term *t = term_new();
    if (!t) {
        fprintf(stderr, "ks_show: no memory\n");
        return 1;
    }

    Win w;
    u32 cols = 80, rows = 24;
    if (!win_open(w, "koru-screen", cols, rows, 0)) {
        fprintf(stderr, "ks_show: %s\n", SDL_GetError());
        return 1;
    }
    screen_resize(*t, cols, rows);
    screen_cursor(*t, true);
    feed_stdin(*t);
    win_present(w, *t);

    if (const char *path = getenv("KORU_SCREEN_SNAP")) {
        int rc = win_snapshot(w, path) ? 0 : 1;
        if (rc)
            fprintf(stderr, "ks_show: %s\n", SDL_GetError());
        win_close(w);
        term_free(t);
        return rc;
    }

    for (bool running = true; running;) {
        SDL_Event e;
        if (!SDL_WaitEvent(&e))
            break;
        switch (e.type) {
        case SDL_EVENT_QUIT:
            running = false;
            break;
        case SDL_EVENT_KEY_DOWN: {
            KsKey k;
            if (!win_key(e.key, k))
                break;
            report(k);
            // ^Q and ^C leave, which is the one thing this tool decides for
            // itself; a program would be told and would decide.
            if ((k.mods & KS_MOD_CTRL) && (k.code == 'q' || k.code == 'c'))
                running = false;
            break;
        }
        case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
        case SDL_EVENT_WINDOW_RESIZED: {
            // The window is pixels and the grid is cells: one conversion, in
            // one place, and the terminal is resized before the renderer is.
            win_grid_of(w, e.window.data1, e.window.data2, cols, rows);
            if (cols != screen(*t).cols || rows != screen(*t).rows) {
                screen_resize(*t, cols, rows);
                win_resize(w, screen(*t).cols, screen(*t).rows);
                win_present(w, *t);
            }
            break;
        }
        case SDL_EVENT_WINDOW_EXPOSED:
            win_present(w, *t);
            break;
        default:
            break;
        }
    }

    win_close(w);
    term_free(t);
    return 0;
}
