// SPDX-License-Identifier: MIT
//
// What every case of the C++ surface's device suite needs: a ring made
// ambient, and a fixture tree under /tmp. Shared by the T45, T46 and T48
// sections, which are three files of the same binary.
//
// The fixture roots are this suite's own, so a Rust run can be in flight at
// the same time.

#ifndef KORU_TESTS_SURFACE_HPP
#define KORU_TESTS_SURFACE_HPP

#include "harness.hpp"

#include <koru/braam.hpp>
#include <koru/file.hpp>

#include <filesystem>
#include <fstream>

/// A ring, a mapping and an executor, installed as the ambient one for as long
/// as this lives. Braam's surface names no executor, so this is what a
/// `koru_main` program is handed.
struct Rt {
    koru::Executor *ex = nullptr;

    Rt()
    {
        koru::result<koru::Ring> r = koru::Ring::with_config(koru::default_config());
        if (!r) {
            FAILF("/dev/koru: %s", r.error().message());
            return;
        }
        koru::Ring ring             = std::move(r).take();
        koru::result<koru::Arena> a = ring.mmap();
        if (!a) {
            FAILF("mmap: %s", a.error().message());
            return;
        }
        ex = new koru::Executor(std::move(ring), std::move(a).take());
        koru::install(*ex);
    }

    ~Rt()
    {
        if (ex) {
            koru::shutdown();
            delete ex;
        }
    }

    Rt(const Rt &)            = delete;
    Rt &operator=(const Rt &) = delete;
};

/// Install a ring and drive `t` on it.
inline void run(koru::task<void> t)
{
    arm_alarm(120);
    Rt rt;
    if (rt.ex)
        rt.ex->run(std::move(t));
    disarm_alarm();
}

/// `ENTER` calls so far. The reactor's own counter, not a guess.
inline uint64_t enters()
{
    return koru::reactor().enters();
}

/// `<root>/<name>`, wiped and remade. Each case owns its own corner, so the
/// order the harness picks does not matter.
inline koru::String fixture(const char *root, const char *name)
{
    koru::String dir = koru::String(root) + "/" + name;
    std::error_code ec;
    std::filesystem::remove_all(dir.c_str(), ec);
    std::filesystem::create_directories(dir.c_str(), ec);
    return dir;
}

inline void put_file(const koru::String &path, const koru::String &text)
{
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    f.write(text.data(), std::streamsize(text.size()));
}

inline koru::String slurp(const koru::String &path)
{
    std::ifstream f(path, std::ios::binary);
    return koru::String((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

#endif // KORU_TESTS_SURFACE_HPP
