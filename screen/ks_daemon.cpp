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
    int fd   = -1;
    Conn *cn = nullptr;
};

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

        for (size_t i = 0; i < clients.size();) {
            Client &c   = clients[i];
            short events = revents_of(c.fd);
            bool gone   = (events & (POLLHUP | POLLERR)) != 0;
            if (events & POLLIN) {
                uint8_t buf[4096];
                ssize_t n = recv(c.fd, buf, sizeof(buf), 0);
                if (n > 0)
                    feed(*c.cn, buf, size_t(n));
                else if (n == 0)
                    gone = true;
                else if (errno != EAGAIN && errno != EINTR)
                    gone = true;
            }
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
