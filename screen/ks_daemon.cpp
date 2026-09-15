// SPDX-License-Identifier: MIT
//
// koru-screen: the daemon. A window, a terminal, and a Unix socket clients
// reach it over.
//
// The rule that keeps this file small: **the protocol server is pure**. This
// loop does recv, feed, drain and nothing else; every decision about a frame
// is in proto.cpp, where a test can drive it with no socket at all.
//
//   koru-screen                       the socket under $XDG_RUNTIME_DIR
//   KORU_SCREEN_SOCK=/tmp/s koru-screen
//   KORU_SCREEN_ONCE=1 koru-screen    serve one connection and exit, for tests
//   KORU_SCREEN_SNAP=/tmp/x.bmp       write every frame there, for tests
//   KORU_SCREEN_KEYS=/tmp/k           type this script in, for tests

#include "proto.h"
#include "window.h"

#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <poll.h>
#include <string>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#include <vector>

namespace {

struct Client {
    int fd    = -1;
    Conn *cn  = nullptr;
    bool gone = false; // the peer went, noticed in the read pass
};

// ------------------------------------------------------------- the key script
//
// KORU_SCREEN_KEYS names a file of one key per line, which this daemon types
// in as if somebody were at the window. It exists because a keystroke has no
// other way in: SDL's events come from a real window, and a shell test has
// none.
//
// **A key is fed only while a client is parked on a KEY_READ.** That makes the
// script self-paced — the program repaints, asks for the next key, and only
// then is it typed — so there is no sleep anywhere in it and no race to lose.

struct KsKeyStroke {
    uint32_t code = 0;
    uint32_t mods = 0;
};

// One token: a named key, `ctrl+x`, or a single character.
bool parse_key(const std::string &word, KsKeyStroke &out)
{
    static const struct {
        const char *name;
        uint32_t code;
    } NAMED[] = {
        { "enter", KS_KEY_ENTER },         { "backspace", KS_KEY_BACKSPACE },
        { "tab", KS_KEY_TAB },             { "escape", KS_KEY_ESCAPE },
        { "delete", KS_KEY_DELETE },       { "insert", KS_KEY_INSERT },
        { "up", KS_KEY_UP },               { "down", KS_KEY_DOWN },
        { "left", KS_KEY_LEFT },           { "right", KS_KEY_RIGHT },
        { "home", KS_KEY_HOME },           { "end", KS_KEY_END },
        { "pgup", KS_KEY_PAGE_UP },        { "pgdn", KS_KEY_PAGE_DOWN },
        { "space", uint32_t(' ') },
    };

    std::string w = word;
    out           = KsKeyStroke{};
    if (w.rfind("ctrl+", 0) == 0) {
        out.mods = KS_MOD_CTRL;
        w        = w.substr(5);
    }
    for (const auto &n : NAMED)
        if (w == n.name) {
            out.code = n.code;
            return true;
        }
    if (w.size() == 1) {
        out.code = uint32_t(static_cast<unsigned char>(w[0]));
        return true;
    }
    return false;
}

std::vector<KsKeyStroke> read_script(const char *path)
{
    std::vector<KsKeyStroke> keys;
    FILE *f = fopen(path, "r");
    if (!f) {
        fprintf(stderr, "koru-screen: %s: %s\n", path, strerror(errno));
        return keys;
    }
    char line[64];
    while (fgets(line, sizeof line, f)) {
        std::string w(line);
        while (!w.empty() && (w.back() == '\n' || w.back() == '\r'))
            w.pop_back();
        if (w.empty() || w[0] == '#')
            continue;
        KsKeyStroke k;
        if (parse_key(w, k))
            keys.push_back(k);
        else
            fprintf(stderr, "koru-screen: unknown key '%s'\n", w.c_str());
    }
    fclose(f);
    return keys;
}

std::string sock_path()
{
    if (const char *p = getenv("KORU_SCREEN_SOCK"))
        return p;
    const char *dir = getenv("XDG_RUNTIME_DIR");
    if (!dir || !*dir)
        dir = "/tmp";
    return std::string(dir) + "/" + KS_SOCK_NAME;
}

// Bind a temporary name beside the socket and rename it into place, so a
// half-initialised socket is never connectable: a client that connects has a
// daemon that is listening.
int listen_on(const std::string &path)
{
    std::string tmp = path + ".tmp." + std::to_string(getpid());
    unlink(tmp.c_str());

    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (fd < 0)
        return -1;

    sockaddr_un a{};
    a.sun_family = AF_UNIX;
    if (tmp.size() >= sizeof(a.sun_path)) {
        close(fd);
        errno = ENAMETOOLONG;
        return -1;
    }
    memcpy(a.sun_path, tmp.c_str(), tmp.size());

    mode_t was = umask(0077); // the socket is the user's own
    int rc     = bind(fd, reinterpret_cast<sockaddr *>(&a), sizeof(a));
    umask(was);
    // A deep backlog: one window is shared by every koru program in the
    // session, and twenty starting at once is the case T38 tests. A short
    // queue answers the overflow with ECONNREFUSED, which a client cannot tell
    // from a dead daemon.
    if (rc < 0 || listen(fd, 128) < 0) {
        close(fd);
        unlink(tmp.c_str());
        return -1;
    }
    if (rename(tmp.c_str(), path.c_str()) < 0) {
        close(fd);
        unlink(tmp.c_str());
        return -1;
    }
    return fd;
}

// Everything this loop does with a connection's replies. A short write is
// normal on a socket, so what was written is what is drained.
void flush_out(Client &c)
{
    for (;;) {
        size_t n         = 0;
        const uint8_t *p = conn_out(*c.cn, n);
        if (!n)
            return;
        ssize_t wrote = send(c.fd, p, n, MSG_NOSIGNAL);
        if (wrote <= 0)
            return; // EAGAIN, or the peer has gone; poll will say which
        conn_drain(*c.cn, size_t(wrote));
    }
}

} // namespace

int main()
{
    signal(SIGPIPE, SIG_IGN); // a client that goes is an EPIPE, not a death

    Term *t = term_new();
    if (!t) {
        fprintf(stderr, "koru-screen: no memory\n");
        return 1;
    }

    Win w;
    u32 cols = 80, rows = 24;
    if (!win_open(w, "koru", cols, rows, 0)) {
        fprintf(stderr, "koru-screen: %s\n", SDL_GetError());
        return 1;
    }
    screen_resize(*t, cols, rows);
    screen_cursor(*t, true);

    Server *srv = server_new(t);
    std::string path = sock_path();
    int lfd          = listen_on(path);
    if (!srv || lfd < 0) {
        fprintf(stderr, "koru-screen: %s: %s\n", path.c_str(), strerror(errno));
        return 1;
    }
    bool once = getenv("KORU_SCREEN_ONCE") != nullptr;
    bool served = false;

    std::vector<KsKeyStroke> script;
    size_t typed = 0;
    if (const char *path = getenv("KORU_SCREEN_KEYS"))
        script = read_script(path);

    std::vector<Client> clients;
    win_present(w, *t);

    for (bool running = true; running;) {
        std::vector<pollfd> fds;
        fds.push_back(pollfd{ lfd, POLLIN, 0 });
        for (const Client &c : clients)
            fds.push_back(pollfd{ c.fd, short(POLLIN), 0 });

        // A short timeout, because SDL's events do not arrive on a descriptor
        // this loop can poll: the window is pumped between waits.
        poll(fds.data(), fds.size(), 16);

        if (fds[0].revents & POLLIN) {
            int fd = accept4(lfd, nullptr, nullptr, SOCK_CLOEXEC | SOCK_NONBLOCK);
            if (fd >= 0) {
                Client c;
                c.fd = fd;
                c.cn = conn_new(*srv);
                if (!c.cn)
                    close(fd);
                else {
                    clients.push_back(c);
                    served = true;
                }
            }
        }

        // By descriptor, not by index: a client accepted just above is not in
        // this poll set, and indexing by position would read past it.
        auto revents_of = [&fds](int fd) -> short {
            for (const pollfd &p : fds)
                if (p.fd == fd)
                    return p.revents;
            return 0;
        };

        // Byte channels first, framed connections second. A client that waits
        // for its write to complete before sending a frame has put its bytes
        // in our socket buffer already, and this is what makes us take them in
        // that order: otherwise a print and the blit after it race.
        for (int pass = 0; pass < 2; pass++)
            for (Client &c : clients) {
                if (conn_is_bytes(*c.cn) != (pass == 0))
                    continue;
                if (!(revents_of(c.fd) & POLLIN))
                    continue;
                uint8_t buf[4096];
                ssize_t n = recv(c.fd, buf, sizeof(buf), 0);
                if (n > 0)
                    feed(*c.cn, buf, size_t(n));
                else if (n == 0)
                    c.gone = true;
                else if (errno != EAGAIN && errno != EINTR)
                    c.gone = true;
            }

        for (size_t i = 0; i < clients.size();) {
            Client &c = clients[i];
            bool gone = c.gone || (revents_of(c.fd) & (POLLHUP | POLLERR)) != 0;
            flush_out(c);
            if (gone || conn_closed(*c.cn)) {
                // The claims go back here — on EOF, on a kill, on a panic,
                // because the kernel closes the socket either way.
                conn_eof(*c.cn);
                conn_free(c.cn);
                close(c.fd);
                clients.erase(clients.begin() + long(i));
                continue;
            }
            i++;
        }

        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            switch (e.type) {
            case SDL_EVENT_QUIT:
                running = false;
                break;
            case SDL_EVENT_KEY_DOWN: {
                KsKey k;
                if (win_key(e.key, k))
                    server_key(*srv, k.code, k.mods);
                break;
            }
            case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
            case SDL_EVENT_WINDOW_RESIZED:
                win_grid_of(w, e.window.data1, e.window.data2, cols, rows);
                if (cols != screen(*t).cols || rows != screen(*t).rows) {
                    server_resize(*srv, cols, rows);
                    win_resize(w, screen(*t).cols, screen(*t).rows);
                }
                break;
            default:
                break;
            }
        }

        // The scripted keys, one per turn and only into a parked reader.
        if (typed < script.size())
            for (const Client &c : clients)
                if (conn_parked(*c.cn)) {
                    server_key(*srv, script[typed].code, script[typed].mods);
                    typed++;
                    break;
                }

        // One present per turn of the loop, and only what changed is drawn.
        if (screen_damage(*t).w) {
            win_present(w, *t);
            // The window is the only record of what a client painted, and a
            // test has none: with this set, every frame is also a file.
            if (const char *snap = getenv("KORU_SCREEN_SNAP"))
                win_snapshot(w, snap);
        }
        for (Client &c : clients)
            flush_out(c);

        if (once && served && clients.empty())
            running = false;
    }

    for (Client &c : clients) {
        conn_free(c.cn);
        close(c.fd);
    }
    close(lfd);
    unlink(path.c_str());
    server_free(srv);
    win_close(w);
    term_free(t);
    return 0;
}
