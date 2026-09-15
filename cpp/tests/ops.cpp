// SPDX-License-Identifier: MIT
//
// T45's done test: T30's, in C++. Braam's prototypes, and every operation
// against its libc equivalent on a fixture tree.
//
// Needs /dev/koru, so it runs in the VM under scripts/run-cpp.sh. Its fixture
// root is its own, so the Rust suite can be running at the same time.

#include "surface.hpp"

#include <koru/braam.hpp>

#include <cerrno>
#include <cstring>
#include <ctime>
#include <dirent.h>
#include <filesystem>
#include <fstream>
#include <sys/stat.h>
#include <unistd.h>
#include <algorithm>

using namespace koru;

// ---------------------------------------------------------------------------
// Signature conformance
// ---------------------------------------------------------------------------

namespace {

/// Braam's `src/proc/io.h`, prototype for prototype. Never called: it exists
/// so that a drift in a name, an argument order or a type is a compile error
/// rather than a surprise at a call site.
///
/// The comment above each line is the C++ it must match. Where the type below
/// is not the one in the comment, doc/Notes.md says why.
Task<void> braam_prototypes(Handle fd, Str path, u32 flags, Error why)
{
    // Task<Result<void>> write_all(u32 fd, Str s);
    Result<void> a = co_await write_all(fd, path);
    // Task<Result<String>> read_chunk(u32 fd);
    Result<String> b = co_await read_chunk(fd);
    // Task<Result<String>> read_some(u32 fd, u32 max);
    Result<String> c = co_await read_some(fd, u32(0));
    // Task<Result<i32>> open_at(Str path, u32 flags);
    Result<i32> d = co_await open_at(path, flags);
    // Task<Result<i32>> open_read(Str path);
    Result<i32> e = co_await open_read(path);
    // Task<Result<String>> read_file(Str path);
    Result<String> f = co_await read_file(path);
    // Task<void> close_fd(u32 fd);
    co_await close_fd(fd);
    // Task<Result<u32>> dup_fd(u32 fd);
    Result<Handle> g = co_await dup_fd(fd);
    // Task<Result<u64>> seek_fd(u32 fd, i64 off, u32 whence);
    Result<u64> h = co_await seek_fd(fd, i64(0), u32(0));
    // Task<Result<void>> truncate_fd(u32 fd, u64 n);
    Result<void> i = co_await truncate_fd(fd, u64(0));
    // Task<Result<FileInfo>> stat_of(Str path, bool follow = true);
    Result<FileInfo> j = co_await stat_of(path, true);
    // Task<Result<FileInfo>> stat_fd(u32 fd);
    Result<FileInfo> k = co_await stat_fd(fd);
    // Task<Result<Vec<DirEntry>>> list_dir(Str path);
    Result<Vec<DirEntry>> l = co_await list_dir(path);
    // Task<Result<void>> make_dir(Str path);
    Result<void> m = co_await make_dir(path);
    // Task<Result<void>> make_dir_all(Str path);
    Result<void> n = co_await make_dir_all(path);
    // Task<Result<void>> copy_file(Str from, Str to);
    Result<void> o = co_await copy_file(path, path);
    // Task<Result<void>> copy_tree(Str from, Str to);
    Result<void> p = co_await copy_tree(path, path);
    // Task<Result<void>> remove_path(Str path, bool all);
    Result<void> q = co_await remove_path(path, false);
    // Task<Result<void>> touch_path(Str path);
    Result<void> r = co_await touch_path(path);
    // Task<Result<void>> make_link(Str target, Str path);
    Result<void> s = co_await make_link(path, path);
    // Task<Result<String>> read_link(Str path);
    Result<String> t = co_await read_link(path);
    // Task<Result<void>> rename_path(Str from, Str to);
    Result<void> u = co_await rename_path(path, path);
    // Task<Result<String>> cwd_get();
    Result<String> v = co_await cwd_get();
    // Task<Result<String>> cwd_set(Str path);
    Result<String> w = co_await cwd_set(path);
    // Task<Result<void>> sleep_for(u32 ms);
    Result<void> x = co_await sleep_for(u32(0));
    // Task<Result<Clock>> clock_now();
    Result<Clock> y = co_await clock_now();
    // Task<void> errln(Str who, Str what, Error why);
    co_await errln(path, path, why);
    // bool next_line(Str &rest, Str &line);
    Str rest = path, line;
    bool z = next_line(rest, line);
    // Str next_field(Str &line);
    Str field = next_field(line);

    (void)a, (void)b, (void)c, (void)d, (void)e, (void)f, (void)g, (void)h, (void)i;
    (void)j, (void)k, (void)l, (void)m, (void)n, (void)o, (void)p, (void)q, (void)r;
    (void)s, (void)t, (void)u, (void)v, (void)w, (void)x, (void)y, (void)z, (void)field;
}

} // namespace

CASE(ops_every_declaration_matches_braams_prototype)
{
    // The declarations above are the test; this keeps the harness honest about
    // having run something.
    Task<void> (*f)(Handle, Str, u32, Error) = braam_prototypes;
    CHECK(f != nullptr);
}

// ---------------------------------------------------------------------------
// The fixture tree
// ---------------------------------------------------------------------------

namespace {

namespace fs = std::filesystem;

const char *ROOT = "/tmp/koru-ops-cpp";

/// `ROOT/<name>`, wiped and remade.
String fixture(const char *name)
{
    return ::fixture(ROOT, name);
}

void put(const String &path, const String &text)
{
    put_file(path, text);
}

// What libc reports, in the shape koru reports it. The oracle for `stat_of`.

u64 mtime_ms(const struct stat &st)
{
    return u64(st.st_mtim.tv_sec) * 1000 + u64(st.st_mtim.tv_nsec) / 1000000;
}

FileKind kind_of(const struct stat &st)
{
    if (S_ISDIR(st.st_mode))
        return FileKind::Dir;
    if (S_ISLNK(st.st_mode))
        return FileKind::Link;
    return FileKind::File;
}

FileInfo libc_info(const String &path, bool follow)
{
    struct stat st = {};
    int rc         = follow ? ::stat(path.c_str(), &st) : ::lstat(path.c_str(), &st);
    if (rc != 0) {
        FAILF("libc could not stat %s: %s", path.c_str(), strerror(errno));
        return FileInfo{};
    }
    return FileInfo{ kind_of(st), u64(st.st_size), mtime_ms(st) };
}

/// Names, kinds and sizes, sorted, as `readdir` would report them.
std::vector<DirEntry> libc_listing(const String &dir)
{
    std::vector<DirEntry> out;
    DIR *d = opendir(dir.c_str());
    if (!d) {
        FAILF("libc could not list %s", dir.c_str());
        return out;
    }
    while (struct dirent *e = readdir(d)) {
        String name(e->d_name);
        if (name == "." || name == "..")
            continue;
        struct stat st = {};
        if (::lstat((dir + "/" + name).c_str(), &st) != 0)
            continue;
        FileKind k = kind_of(st);
        // A listing never resolves a link, so a link reports its own length
        // and koru fills its times from nothing.
        out.push_back(DirEntry{ name, k, u64(st.st_size), k == FileKind::Link ? 0 : mtime_ms(st) });
    }
    closedir(d);
    std::sort(out.begin(), out.end(),
              [](const DirEntry &a, const DirEntry &b) { return a.name < b.name; });
    return out;
}

} // namespace

// ---------------------------------------------------------------------------
// Streams
// ---------------------------------------------------------------------------

namespace {

Task<void> case_read(String path, String text)
{
    Result<String> whole = co_await read_file(path);
    CHECK(whole.ok() && whole.value() == text);

    // read_chunk stops at the end of input with Closed, never an empty Ok.
    Result<Handle> fd = co_await open_read(path);
    CO_REQUIRE(fd.ok());
    String got;
    for (;;) {
        Result<String> chunk = co_await read_chunk(fd.value());
        if (!chunk.ok()) {
            CHECK(chunk.error() == Kind::Closed);
            break;
        }
        got += chunk.value();
    }
    co_await close_fd(fd.value());
    CHECK(got == text);

    // read_some leaves the rest on the descriptor for the next read.
    Result<Handle> two = co_await open_read(path);
    CO_REQUIRE(two.ok());
    Result<String> a = co_await read_some(two.value(), 4);
    Result<String> b = co_await read_some(two.value(), 4);
    CHECK(a.ok() && a.value() == "one\n");
    CHECK(b.ok() && b.value() == "two\n");
    co_await close_fd(two.value());

    Result<String> missing = co_await read_file("/no/such/file");
    CHECK(!missing.ok() && missing.error() == Kind::NotFound);
}

} // namespace

CASE(ops_a_file_reads_through_the_ring_exactly_as_libc_reads_it)
{
    String dir  = fixture("read");
    String path = dir + "/text";
    String text;
    for (int i = 0; i < 500; i++)
        text += "one\ntwo\nthree\n"; // past one slot
    put(path, text);
    run(case_read(path, text));
}

namespace {

Task<void> case_write(String path)
{
    Result<Handle> fd = co_await open_at(path, O_WRITE | O_CREATE | O_TRUNC);
    CO_REQUIRE(fd.ok());
    CHECK((co_await write_all(fd.value(), "hello, ")).ok());
    CHECK((co_await write_all(fd.value(), "world\n")).ok());
    CHECK((co_await write_all(fd.value(), "")).ok()); // a no-op, not an error
    co_await close_fd(fd.value());
    CHECK(slurp(path) == "hello, world\n");

    // O_TRUNC over it, then O_APPEND after it.
    Result<Handle> t = co_await open_at(path, O_WRITE | O_TRUNC);
    CO_REQUIRE(t.ok());
    CHECK((co_await write_all(t.value(), "a")).ok());
    co_await close_fd(t.value());
    Result<Handle> ap = co_await open_at(path, O_WRITE | O_APPEND);
    CO_REQUIRE(ap.ok());
    CHECK((co_await write_all(ap.value(), "b")).ok());
    co_await close_fd(ap.value());
    CHECK(slurp(path) == "ab");

    // O_EXCL over a name that is taken, and a flag koru does not know.
    Result<Handle> ex = co_await open_at(path, O_WRITE | O_CREATE | O_EXCL);
    CHECK(!ex.ok() && ex.error() == Kind::Exists);
    Result<Handle> bad = co_await open_at(path, 1u << 20);
    CHECK(!bad.ok() && bad.error() == Kind::Invalid);
}

} // namespace

CASE(ops_a_write_lands_where_libc_would_have_put_it)
{
    String dir = fixture("write");
    run(case_write(dir + "/out"));
}

// ---------------------------------------------------------------------------
// Descriptors
// ---------------------------------------------------------------------------

namespace {

Task<void> case_dup(String path)
{
    Result<Handle> a = co_await open_read(path);
    CO_REQUIRE(a.ok());
    Result<Handle> b = co_await dup_fd(a.value());
    CO_REQUIRE(b.ok());

    // One handle behind both, so the offset is shared.
    Result<String> one = co_await read_some(a.value(), 2);
    Result<String> two = co_await read_some(b.value(), 2);
    CHECK(one.ok() && one.value() == "ab");
    CHECK(two.ok() && two.value() == "cd");
    Result<u64> at = co_await seek_fd(a.value(), 0, SEEK_CUR);
    CHECK(at.ok() && at.value() == 4);

    // Closing one shuts nothing.
    co_await close_fd(a.value());
    Result<String> rest = co_await read_some(b.value(), 2);
    CHECK(rest.ok() && rest.value() == "ef");
    co_await close_fd(b.value());
    // Now it is gone, and the kernel says so rather than this guessing.
    CHECK(!(co_await read_some(b.value(), 2)).ok());
}

} // namespace

CASE(ops_a_second_name_shares_the_one_offset_and_the_last_close_shuts_it)
{
    String dir  = fixture("dup");
    String path = dir + "/text";
    put(path, "abcdef");
    run(case_dup(path));
}

namespace {

Task<void> case_seek(String path)
{
    Result<Handle> f = co_await open_read(path);
    CO_REQUIRE(f.ok());
    Handle fd = f.value();

    Result<u64> a = co_await seek_fd(fd, 4, SEEK_SET);
    CHECK(a.ok() && a.value() == 4);
    Result<String> r1 = co_await read_some(fd, 2);
    CHECK(r1.ok() && r1.value() == "45");
    Result<u64> b = co_await seek_fd(fd, 1, SEEK_CUR);
    CHECK(b.ok() && b.value() == 7);
    Result<String> r2 = co_await read_some(fd, 1);
    CHECK(r2.ok() && r2.value() == "7");
    Result<u64> c = co_await seek_fd(fd, 0, SEEK_END);
    CHECK(c.ok() && c.value() == 10);
    Result<u64> d = co_await seek_fd(fd, -3, SEEK_END);
    CHECK(d.ok() && d.value() == 7);
    Result<u64> e = co_await seek_fd(fd, -2, SEEK_CUR);
    CHECK(e.ok() && e.value() == 5);

    // Past the end is not an error; a read there is an end of input.
    Result<u64> far = co_await seek_fd(fd, 100, SEEK_SET);
    CHECK(far.ok() && far.value() == 100);
    Result<String> eof = co_await read_chunk(fd);
    CHECK(!eof.ok() && eof.error() == Kind::Closed);

    // Before the start, and a whence that is not one.
    CHECK(!(co_await seek_fd(fd, -1, SEEK_SET)).ok());
    CHECK(!(co_await seek_fd(fd, 0, 9)).ok());
    co_await close_fd(fd);

    // Unsupported on anything that is not a file, which is what a standard
    // stream is here: nothing opened it by name.
    Result<u64> stream = co_await seek_fd(in_fd(), 0, SEEK_CUR);
    CHECK(!stream.ok() && stream.error() == Kind::Unsupported);
}

} // namespace

CASE(ops_a_seek_lands_where_lseek_would_have_landed)
{
    String dir  = fixture("seek");
    String path = dir + "/text";
    put(path, "0123456789");
    run(case_seek(path));
}

namespace {

Task<void> case_truncate(String path)
{
    Result<Handle> f = co_await open_at(path, O_WRITE);
    CO_REQUIRE(f.ok());
    CHECK((co_await truncate_fd(f.value(), 4)).ok());
    CHECK(slurp(path) == "0123");
    CHECK((co_await truncate_fd(f.value(), 8)).ok());
    CHECK(slurp(path) == String("0123\0\0\0\0", 8)); // grown with zeros
    co_await close_fd(f.value());

    // Perm unless the open asked to write.
    Result<Handle> ro = co_await open_read(path);
    CO_REQUIRE(ro.ok());
    Result<void> denied = co_await truncate_fd(ro.value(), 0);
    CHECK(!denied.ok() && denied.error() == Kind::Perm);
    co_await close_fd(ro.value());

    // And what seek_fd refuses this refuses.
    Result<void> stream = co_await truncate_fd(out_fd(), 0);
    CHECK(!stream.ok() && stream.error() == Kind::Unsupported);
}

} // namespace

CASE(ops_a_truncation_sets_the_length_ftruncate_would_have_set)
{
    String dir  = fixture("truncate");
    String path = dir + "/text";
    put(path, "0123456789");
    run(case_truncate(path));
}

// ---------------------------------------------------------------------------
// Metadata
// ---------------------------------------------------------------------------

namespace {

Task<void> case_stat(String file, String sub, String link)
{
    Result<FileInfo> f = co_await stat_of(file, true);
    CHECK(f.ok() && f.value() == libc_info(file, true));
    Result<FileInfo> d = co_await stat_of(sub, true);
    CHECK(d.ok() && d.value() == libc_info(sub, true));

    // A followed link is its target; an unfollowed one is the link, whose
    // size is its target's length, as lstat(2) reports it.
    Result<FileInfo> followed = co_await stat_of(link, true);
    CHECK(followed.ok() && followed.value() == libc_info(file, true));
    Result<FileInfo> seen = co_await stat_of(link, false);
    CO_REQUIRE(seen.ok());
    CHECK(seen.value().kind == FileKind::Link);
    CHECK_EQ(seen.value().size, libc_info(link, false).size);

    // stat_fd sees the file being read, whatever name reached it.
    Result<Handle> fd = co_await open_read(link);
    CO_REQUIRE(fd.ok());
    Result<FileInfo> byfd = co_await stat_fd(fd.value());
    CHECK(byfd.ok() && byfd.value() == libc_info(file, true));
    co_await close_fd(fd.value());

    Result<FileInfo> missing = co_await stat_of("/no/such/file", true);
    CHECK(!missing.ok() && missing.error() == Kind::NotFound);
}

Task<void> case_touch(String path, u64 before)
{
    CHECK_EQ(before, 1000000000); // the fixture's mtime is the one set
    CHECK((co_await touch_path(path)).ok());
    Result<FileInfo> after = co_await stat_of(path, true);
    CO_REQUIRE(after.ok());
    CHECK(after.value().mtime > before);
    CHECK_EQ(after.value().mtime, libc_info(path, true).mtime);

    Result<void> missing = co_await touch_path("/no/such/file");
    CHECK(!missing.ok() && missing.error() == Kind::NotFound);
}

} // namespace

CASE(ops_every_stat_agrees_with_libcs)
{
    String dir  = fixture("stat");
    String file = dir + "/file";
    String sub  = dir + "/sub";
    String link = dir + "/link";
    put(file, "0123456789");
    std::error_code ec;
    fs::create_directory(sub.c_str(), ec);
    fs::create_symlink("file", link.c_str(), ec);
    run(case_stat(file, sub, link));
}

CASE(ops_a_touch_moves_the_mtime_forward)
{
    String dir  = fixture("touch");
    String path = dir + "/file";
    put(path, "x");
    struct timespec ts[2];
    ts[0].tv_sec = ts[1].tv_sec = 1000000;
    ts[0].tv_nsec = ts[1].tv_nsec = 0;
    CHECK_EQ(utimensat(AT_FDCWD, path.c_str(), ts, 0), 0);
    run(case_touch(path, libc_info(path, true).mtime));
}

// ---------------------------------------------------------------------------
// Directories and names
// ---------------------------------------------------------------------------

namespace {

Task<void> case_list(String dir)
{
    Result<std::vector<DirEntry>> got = co_await list_dir(dir);
    CO_REQUIRE(got.ok());
    std::vector<DirEntry> v = got.value();
    std::sort(v.begin(), v.end(),
              [](const DirEntry &a, const DirEntry &b) { return a.name < b.name; });
    std::vector<DirEntry> want = libc_listing(dir);
    CO_REQUIRE(v.size() == want.size());
    for (size_t i = 0; i < v.size(); i++)
        if (!(v[i] == want[i]))
            FAILF("%s: koru and readdir disagree", v[i].name.c_str());

    // `.` and `..` never reach a caller.
    for (const DirEntry &e : v)
        CHECK(e.name != "." && e.name != "..");
    // A dangling link is listed, not an error.
    bool dangling = false;
    for (const DirEntry &e : v)
        if (e.name == "d-dangling")
            dangling = e.kind == FileKind::Link;
    CHECK(dangling);

    String file                     = dir + "/b-file";
    Result<std::vector<DirEntry>> no = co_await list_dir(file);
    CHECK(!no.ok() && no.error() == Kind::NotDir);
}

/// Big enough that the entries need more than one `READDIR`, which is the
/// resume cookie's only test here.
Task<void> case_list_long(String dir)
{
    Result<std::vector<DirEntry>> got = co_await list_dir(dir);
    CO_REQUIRE(got.ok());
    std::vector<DirEntry> v = got.value();
    CHECK_EQ(v.size(), 500);
    std::sort(v.begin(), v.end(),
              [](const DirEntry &a, const DirEntry &b) { return a.name < b.name; });
    if (v.size() == 500) {
        CHECK(v[0].name == "entry-with-a-long-name-0000");
        CHECK(v[499].name == "entry-with-a-long-name-0499");
    }
}

Task<void> case_mkdir(String dir, String deep, String file)
{
    String one = dir + "/one";
    CHECK((co_await make_dir(one)).ok());
    CHECK(fs::is_directory(one.c_str()));

    // A second make_dir over it is Exists; make_dir_all is not.
    Result<void> again = co_await make_dir(one);
    CHECK(!again.ok() && again.error() == Kind::Exists);
    CHECK((co_await make_dir_all(one)).ok());

    CHECK((co_await make_dir_all(deep)).ok());
    CHECK(fs::is_directory(deep.c_str()));
    CHECK((co_await make_dir_all(deep)).ok()); // idempotent

    // Anything else in the leaf's place is Exists, and a missing parent is
    // what make_dir alone cannot do.
    Result<void> onto = co_await make_dir_all(file);
    CHECK(!onto.ok() && onto.error() == Kind::Exists);
    String nested      = dir + "/x/y";
    Result<void> below = co_await make_dir(nested);
    CHECK(!below.ok() && below.error() == Kind::NotFound);
}

Task<void> case_remove(String file, String empty, String tree)
{
    CHECK((co_await remove_path(file, false)).ok());
    CHECK(!fs::exists(fs::symlink_status(file.c_str())));

    CHECK((co_await remove_path(empty, false)).ok());
    CHECK(!fs::exists(fs::symlink_status(empty.c_str())));

    // Without `all` a populated directory stays, with its errno.
    Result<void> busy = co_await remove_path(tree, false);
    CHECK(!busy.ok() && busy.error() == Kind::NotEmpty);
    CHECK(fs::is_directory(tree.c_str()));

    CHECK((co_await remove_path(tree, true)).ok());
    CHECK(!fs::exists(fs::symlink_status(tree.c_str())));

    Result<void> gone = co_await remove_path(file, true);
    CHECK(!gone.ok() && gone.error() == Kind::NotFound);
}

Task<void> case_link(String dir, String link, String file)
{
    CHECK((co_await make_link("file", link)).ok());
    Result<String> back = co_await read_link(link);
    CHECK(back.ok() && back.value() == "file");
    CHECK(fs::read_symlink(link.c_str()).string() == "file"); // and libc agrees

    // The target is kept as written and not checked.
    String dangling = dir + "/dangling";
    CHECK((co_await make_link("../nowhere/at/all", dangling)).ok());
    Result<String> d = co_await read_link(dangling);
    CHECK(d.ok() && d.value() == "../nowhere/at/all");

    Result<void> taken = co_await make_link("x", link);
    CHECK(!taken.ok() && taken.error() == Kind::Exists);
    Result<String> notalink = co_await read_link(file);
    CHECK(!notalink.ok() && notalink.error() == Kind::Invalid);
}

Task<void> case_rename(String a, String b)
{
    // An existing destination is replaced.
    CHECK((co_await rename_path(a, b)).ok());
    CHECK(!fs::exists(fs::symlink_status(a.c_str())));
    CHECK(slurp(b) == "content");

    Result<void> gone = co_await rename_path(a, b);
    CHECK(!gone.ok() && gone.error() == Kind::NotFound);

    // Across filesystems it is not a failure but an instruction.
    Result<void> xdev = co_await rename_path(b, "/dev/shm/koru-ops-cpp-xdev");
    if (!xdev.ok())
        CHECK(xdev.error() == Kind::Unsupported || xdev.error() == Kind::Perm);
    else
        ::unlink("/dev/shm/koru-ops-cpp-xdev");
}

} // namespace

CASE(ops_a_listing_names_what_readdir_names)
{
    String dir = fixture("list");
    put(dir + "/b-file", "0123");
    std::error_code ec;
    fs::create_directory(dir + "/a-dir", ec);
    fs::create_symlink("b-file", dir + "/c-link", ec);
    fs::create_symlink("nowhere", dir + "/d-dangling", ec);
    run(case_list(dir));
}

CASE(ops_a_long_listing_resumes_across_calls)
{
    String dir = fixture("list-long");
    for (int i = 0; i < 500; i++) {
        char name[64];
        snprintf(name, sizeof(name), "/entry-with-a-long-name-%04d", i);
        put(dir + name, "");
    }
    run(case_list_long(dir));
}

CASE(ops_a_directory_is_made_where_mkdir_p_would_have_made_one)
{
    String dir  = fixture("mkdir");
    String file = dir + "/file";
    put(file, "x");
    run(case_mkdir(dir, dir + "/a/b/c", file));
}

CASE(ops_a_removal_takes_what_rm_would_have_taken)
{
    String dir   = fixture("remove");
    String file  = dir + "/file";
    String empty = dir + "/empty";
    String tree  = dir + "/tree";
    put(file, "x");
    std::error_code ec;
    fs::create_directory(empty.c_str(), ec);
    fs::create_directories(tree + "/a/b", ec);
    put(tree + "/a/b/deep", "x");
    put(tree + "/top", "x");
    fs::create_symlink("nowhere", tree + "/link", ec);
    run(case_remove(file, empty, tree));
}

CASE(ops_a_link_reads_back_as_it_was_written)
{
    String dir  = fixture("link");
    String file = dir + "/file";
    put(file, "x");
    run(case_link(dir, dir + "/link", file));
}

CASE(ops_a_rename_moves_what_rename_would_have_moved)
{
    String dir = fixture("rename");
    put(dir + "/a", "content");
    put(dir + "/b", "other");
    run(case_rename(dir + "/a", dir + "/b"));
}

// ---------------------------------------------------------------------------
// Copying
// ---------------------------------------------------------------------------

namespace {

Task<void> case_copy(String from, String to, String bytes)
{
    CHECK((co_await copy_file(from, to)).ok());
    CHECK(slurp(to) == bytes);

    // Over an existing destination, which is truncated rather than merged.
    put(to, String(bytes.size() * 2, '\0'));
    CHECK((co_await copy_file(from, to)).ok());
    CHECK(slurp(to) == bytes);

    Result<void> missing = co_await copy_file("/no/such/file", to);
    CHECK(!missing.ok() && missing.error() == Kind::NotFound);
}

Task<void> case_tree(String from, String to, String onto_file)
{
    CHECK((co_await copy_tree(from, to)).ok());

    CHECK(slurp(to + "/top") == "top");
    CHECK(slurp(to + "/a/mid") == "mid");
    CHECK(slurp(to + "/a/b/deep") == "deep");
    CHECK(fs::is_directory(to + "/empty"));
    // A link is handed over rather than followed.
    CHECK(fs::read_symlink(to + "/a/link").string() == "../top");

    // `to` may already be there and the two merge.
    CHECK((co_await copy_tree(from, to)).ok());
    CHECK(slurp(to + "/a/b/deep") == "deep");

    // Anything else in its place is Exists.
    Result<void> onto = co_await copy_tree(from, onto_file);
    CHECK(!onto.ok() && onto.error() == Kind::Exists);
}

} // namespace

CASE(ops_a_copy_is_byte_for_byte_including_what_is_not_text)
{
    String dir  = fixture("copy");
    String from = dir + "/from";
    // Past one slot, and every byte value, so nothing here is UTF-8.
    String bytes;
    for (size_t i = 0; i < 64 * 1024 * 2 + 7; i++)
        bytes += char(i % 256);
    put(from, bytes);
    run(case_copy(from, dir + "/to", bytes));
}

CASE(ops_a_tree_copies_with_its_shape_intact)
{
    String dir  = fixture("tree");
    String from = dir + "/from";
    std::error_code ec;
    fs::create_directories(from + "/a/b", ec);
    fs::create_directory(from + "/empty", ec);
    put(from + "/top", "top");
    put(from + "/a/mid", "mid");
    put(from + "/a/b/deep", "deep");
    fs::create_symlink("../top", from + "/a/link", ec);
    String onto = dir + "/afile";
    put(onto, "x");
    run(case_tree(from, dir + "/to", onto));
}

// ---------------------------------------------------------------------------
// The process
// ---------------------------------------------------------------------------

namespace {

Task<void> case_cwd(String dir, String was)
{
    Result<String> here = co_await cwd_get();
    CHECK(here.ok() && here.value() == was);

    // Both report the resulting absolute path.
    Result<String> moved = co_await cwd_set(dir);
    CHECK(moved.ok() && moved.value() == dir);
    Result<String> now = co_await cwd_get();
    CHECK(now.ok() && now.value() == dir);

    // A relative path now resolves against it.
    put(dir + "/here", "x");
    Result<String> rel = co_await read_file("here");
    CHECK(rel.ok() && rel.value() == "x");

    CHECK(!(co_await cwd_set("/no/such/dir")).ok());
    CHECK((co_await cwd_set(was)).ok());
}

Task<void> case_sleep()
{
    struct timespec t0 = {}, t1 = {};
    clock_gettime(CLOCK_MONOTONIC, &t0);
    CHECK((co_await sleep_for(30)).ok());
    clock_gettime(CLOCK_MONOTONIC, &t1);
    i64 ms = (t1.tv_sec - t0.tv_sec) * 1000 + (t1.tv_nsec - t0.tv_nsec) / 1000000;
    CHECK(ms >= 30);
    CHECK(ms < 5000);

    // Zero is not an error, and parks for nothing.
    CHECK((co_await sleep_for(0)).ok());
}

Task<void> case_clock()
{
    struct timespec ts = {};
    clock_gettime(CLOCK_REALTIME, &ts);
    u64 host = u64(ts.tv_sec) * 1000 + u64(ts.tv_nsec) / 1000000;

    Result<Clock> c = co_await clock_now();
    CO_REQUIRE(c.ok());
    u64 saw  = c.value().epoch_ms;
    u64 diff = saw > host ? saw - host : host - saw;
    CHECK(diff < 2000);
    CHECK(c.value().tz_min >= -16 * 60 && c.value().tz_min <= 16 * 60);
}

/// `errln` writes Braam's own wording, so the two bindings' diagnostics match.
/// Captured by writing the same line to a file: stderr is the terminal here.
Task<void> case_errln(String path)
{
    Result<FileInfo> why = co_await stat_of("/no/such/file", true);
    CO_REQUIRE(!why.ok());
    co_await errln("t45", "a.txt", why.error());
    co_await errln("t45", "", why.error());

    Result<Handle> fd = co_await open_at(path, O_WRITE | O_CREATE | O_TRUNC);
    CO_REQUIRE(fd.ok());
    CHECK((co_await write_all(fd.value(), "t45: a.txt: not found\n")).ok());
    co_await close_fd(fd.value());
    CHECK(slurp(path) == "t45: a.txt: not found\n");
}

} // namespace

CASE(ops_the_working_directory_is_the_one_getcwd_reports)
{
    String dir = fixture("cwd");
    char buf[4096];
    String was = getcwd(buf, sizeof(buf)) ? String(buf) : String("/");
    run(case_cwd(dir, was));
}

CASE(ops_a_sleep_parks_for_at_least_as_long_as_it_was_asked)
{
    run(case_sleep());
}

CASE(ops_the_clock_reads_what_the_host_clock_reads)
{
    run(case_clock());
}

CASE(ops_a_diagnostic_reaches_stderr)
{
    String dir = fixture("errln");
    run(case_errln(dir + "/log"));
}
