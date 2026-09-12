// SPDX-License-Identifier: MIT
//
// The canonical ABI dump, C++ side. Must be byte-identical to
// rust/koru-sys/src/bin/abi_dump.rs, which defines the grammar;
// scripts/abi.sh diffs the two.

#include <koru_abi.h>
#include <koru_errno.h>

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <string>

// Bumped when the grammar changes, which is an edit to both emitters.
#define DUMP_VERSION 1

namespace {

std::string out;

struct Counts {
    unsigned long long consts  = 0;
    unsigned long long ioctls  = 0;
    unsigned long long opcodes = 0;
    unsigned long long structs = 0;
    unsigned long long fields  = 0;
    unsigned long long handles = 0;
    unsigned long long kinds   = 0;
    unsigned long long errnos  = 0;
};

Counts counts;

// Every number goes through %llu, so there is one integer conversion here.
std::string fmt(const char *f, ...)
{
    char buf[512];
    va_list ap;
    va_start(ap, f);
    int n = vsnprintf(buf, sizeof(buf), f, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= sizeof(buf)) {
        fprintf(stderr, "abi_dump: record does not fit\n");
        exit(1);
    }
    return std::string(buf, (size_t)n);
}

void konst(const char *name, const char *type, unsigned long long v)
{
    out += fmt("const %s %s %llu\n", name, type, v);
    counts.consts++;
}

void emit_struct(const char *name, unsigned long long size, unsigned long long align,
                 const std::string &body, unsigned long long n, unsigned long long sum)
{
    // Catches a field dropped from both emitters at once.
    if (sum != size) {
        fprintf(stderr, "abi_dump: %s: field sizes sum to %llu, sizeof is %llu\n", name, sum, size);
        exit(1);
    }
    out += fmt("struct %s size %llu align %llu fields %llu\n", name, size, align, n);
    out += body;
    counts.structs++;
    counts.fields += n;
}

} // namespace

// The type name is spelled out, not derived: it is the only thing here that
// catches res turning unsigned.
#define FIELD(s, f, t)                                                     \
    do {                                                                   \
        unsigned long long fsz = sizeof(((struct s *)0)->f);               \
        body += fmt("field %s.%s offset %llu size %llu type %s\n", #s, #f, \
                    (unsigned long long)offsetof(struct s, f), fsz, t);    \
        n++;                                                               \
        sum += fsz;                                                        \
    } while (0)

#define BEGIN_FIELDS() \
    std::string body;  \
    unsigned long long n = 0, sum = 0

#define END_FIELDS(s) emit_struct(#s, sizeof(struct s), alignof(struct s), body, n, sum)

int main()
{
    out += fmt("abi-dump %llu\n", (unsigned long long)DUMP_VERSION);
    out += fmt("dev %s\n", KORU_DEV);

    konst("KORU_MAGIC", "u32", KORU_MAGIC);
    konst("KORU_ABI_VERSION", "u32", KORU_ABI_VERSION);
    konst("KORU_IOC_TYPE", "u32", KORU_IOC_TYPE);
    konst("KORU_NR_SETUP", "u32", KORU_NR_SETUP);
    konst("KORU_NR_GET_PARAMS", "u32", KORU_NR_GET_PARAMS);
    konst("KORU_NR_ENTER", "u32", KORU_NR_ENTER);
    konst("KORU_SETUP_FLAGS_ALL", "u32", KORU_SETUP_FLAGS_ALL);
    konst("KORU_ENTER_FLAGS_ALL", "u32", KORU_ENTER_FLAGS_ALL);
    konst("KORU_SQE_FLAGS_ALL", "u8", KORU_SQE_FLAGS_ALL);
    konst("KORU_MAX_SQ_ENTRIES", "u32", KORU_MAX_SQ_ENTRIES);
    konst("KORU_MAX_CQ_ENTRIES", "u32", KORU_MAX_CQ_ENTRIES);
    konst("KORU_MAX_SLOT_SIZE", "u32", KORU_MAX_SLOT_SIZE);
    konst("KORU_MAX_SLOT_COUNT", "u32", KORU_MAX_SLOT_COUNT);
    konst("KORU_MAX_ARENA_BYTES", "u64", KORU_MAX_ARENA_BYTES);
    konst("KORU_MAX_HANDLES", "u32", KORU_MAX_HANDLES);
    konst("KORU_DEFAULT_HANDLES", "u32", KORU_DEFAULT_HANDLES);
    konst("KORU_MAX_DELAY_NS", "u64", KORU_MAX_DELAY_NS);
    konst("KORU_O_ACCMODE", "u32", KORU_O_ACCMODE);
    konst("KORU_O_RDONLY", "u32", KORU_O_RDONLY);
    konst("KORU_O_WRONLY", "u32", KORU_O_WRONLY);
    konst("KORU_O_RDWR", "u32", KORU_O_RDWR);
    konst("KORU_O_NOFOLLOW", "u32", KORU_O_NOFOLLOW);
    konst("KORU_O_DIRECTORY", "u32", KORU_O_DIRECTORY);
    konst("KORU_O_NONBLOCK", "u32", KORU_O_NONBLOCK);
    konst("KORU_OPEN_FLAGS_ALL", "u32", KORU_OPEN_FLAGS_ALL);
    konst("KORU_CQE_F_MORE", "u32", KORU_CQE_F_MORE);
    konst("KORU_POLL_IN", "u32", KORU_POLL_IN);
    konst("KORU_POLL_OUT", "u32", KORU_POLL_OUT);
    konst("KORU_POLL_PRI", "u32", KORU_POLL_PRI);
    konst("KORU_POLL_RDHUP", "u32", KORU_POLL_RDHUP);
    konst("KORU_POLL_ERR", "u32", KORU_POLL_ERR);
    konst("KORU_POLL_HUP", "u32", KORU_POLL_HUP);
    konst("KORU_POLL_EVENTS_ALL", "u32", KORU_POLL_EVENTS_ALL);
    konst("KORU_S_IFMT", "u64", KORU_S_IFMT);
    konst("KORU_S_IFIFO", "u64", KORU_S_IFIFO);
    konst("KORU_S_IFCHR", "u64", KORU_S_IFCHR);
    konst("KORU_S_IFDIR", "u64", KORU_S_IFDIR);
    konst("KORU_S_IFBLK", "u64", KORU_S_IFBLK);
    konst("KORU_S_IFREG", "u64", KORU_S_IFREG);
    konst("KORU_S_IFLNK", "u64", KORU_S_IFLNK);
    konst("KORU_S_IFSOCK", "u64", KORU_S_IFSOCK);
    konst("KORU_STAT_INO", "u64", KORU_STAT_INO);
    konst("KORU_STAT_SIZE", "u64", KORU_STAT_SIZE);
    konst("KORU_STAT_BLOCKS", "u64", KORU_STAT_BLOCKS);
    konst("KORU_STAT_BLKSIZE", "u64", KORU_STAT_BLKSIZE);
    konst("KORU_STAT_NLINK", "u64", KORU_STAT_NLINK);
    konst("KORU_STAT_TYPE", "u64", KORU_STAT_TYPE);
    konst("KORU_STAT_MODE", "u64", KORU_STAT_MODE);
    konst("KORU_STAT_UID", "u64", KORU_STAT_UID);
    konst("KORU_STAT_GID", "u64", KORU_STAT_GID);
    konst("KORU_STAT_DEV", "u64", KORU_STAT_DEV);
    konst("KORU_STAT_RDEV", "u64", KORU_STAT_RDEV);
    konst("KORU_STAT_ATIME", "u64", KORU_STAT_ATIME);
    konst("KORU_STAT_MTIME", "u64", KORU_STAT_MTIME);
    konst("KORU_STAT_CTIME", "u64", KORU_STAT_CTIME);
    konst("KORU_STAT_BTIME", "u64", KORU_STAT_BTIME);
    konst("KORU_STAT_ALL", "u64", KORU_STAT_ALL);

#define IOCTL(name)                                                     \
    do {                                                                \
        out += fmt("ioctl %s %llu\n", #name, (unsigned long long)name); \
        counts.ioctls++;                                                \
    } while (0)
    IOCTL(KORU_IOC_SETUP);
    IOCTL(KORU_IOC_GET_PARAMS);
    IOCTL(KORU_IOC_ENTER);
#undef IOCTL

#define OPCODE(name)                                                     \
    do {                                                                 \
        out += fmt("opcode %s %llu\n", #name, (unsigned long long)name); \
        counts.opcodes++;                                                \
    } while (0)
    OPCODE(KORU_OP_NOP);
    OPCODE(KORU_OP_DELAY_NS);
    OPCODE(KORU_OP_OPEN);
    OPCODE(KORU_OP_READ);
    OPCODE(KORU_OP_CLOSE);
    OPCODE(KORU_OP_CANCEL);
    OPCODE(KORU_OP_CHECKSUM);
    OPCODE(KORU_OP_WRITE);
    OPCODE(KORU_OP_ADOPT_FD);
    OPCODE(KORU_OP_POLL_ADD);
    OPCODE(KORU_OP_STAT);
#undef OPCODE

    {
        BEGIN_FIELDS();
        FIELD(koru_params, magic, "u32");
        FIELD(koru_params, abi_version, "u32");
        FIELD(koru_params, flags, "u32");
        FIELD(koru_params, sq_entries, "u32");
        FIELD(koru_params, cq_entries, "u32");
        FIELD(koru_params, slot_size, "u32");
        FIELD(koru_params, slot_count, "u32");
        FIELD(koru_params, configured, "u32");
        FIELD(koru_params, features, "u64");
        FIELD(koru_params, arena_size, "u64");
        FIELD(koru_params, max_sq_entries, "u32");
        FIELD(koru_params, max_cq_entries, "u32");
        FIELD(koru_params, max_slot_size, "u32");
        FIELD(koru_params, max_slot_count, "u32");
        FIELD(koru_params, max_arena_bytes, "u64");
        FIELD(koru_params, handle_count, "u32");
        FIELD(koru_params, max_handles, "u32");
        FIELD(koru_params, max_delay_ns, "u64");
        FIELD(koru_params, reserved, "u64[2]");
        END_FIELDS(koru_params);
    }
    {
        BEGIN_FIELDS();
        FIELD(koru_sqe, opcode, "u8");
        FIELD(koru_sqe, flags, "u8");
        FIELD(koru_sqe, rsvd0, "u16");
        FIELD(koru_sqe, len, "u32");
        FIELD(koru_sqe, off, "u64");
        FIELD(koru_sqe, user_data, "u64");
        FIELD(koru_sqe, slot, "u32");
        FIELD(koru_sqe, handle, "u32");
        END_FIELDS(koru_sqe);
    }
    {
        BEGIN_FIELDS();
        FIELD(koru_cqe, user_data, "u64");
        FIELD(koru_cqe, res, "i64");
        FIELD(koru_cqe, flags, "u32");
        FIELD(koru_cqe, rsvd0, "u32");
        FIELD(koru_cqe, extra, "u64");
        END_FIELDS(koru_cqe);
    }
    {
        BEGIN_FIELDS();
        FIELD(koru_enter, sq_addr, "u64");
        FIELD(koru_enter, cq_addr, "u64");
        FIELD(koru_enter, timeout_ns, "u64");
        FIELD(koru_enter, to_submit, "u32");
        FIELD(koru_enter, cq_space, "u32");
        FIELD(koru_enter, min_complete, "u32");
        FIELD(koru_enter, flags, "u32");
        FIELD(koru_enter, completed, "u32");
        FIELD(koru_enter, submitted, "u32");
        FIELD(koru_enter, reserved, "u64[2]");
        END_FIELDS(koru_enter);
    }
    {
        BEGIN_FIELDS();
        FIELD(koru_stat, ino, "u64");
        FIELD(koru_stat, size, "u64");
        FIELD(koru_stat, blocks, "u64");
        FIELD(koru_stat, blksize, "u64");
        FIELD(koru_stat, nlink, "u64");
        FIELD(koru_stat, mode, "u64");
        FIELD(koru_stat, uid, "u64");
        FIELD(koru_stat, gid, "u64");
        FIELD(koru_stat, dev_major, "u64");
        FIELD(koru_stat, dev_minor, "u64");
        FIELD(koru_stat, rdev_major, "u64");
        FIELD(koru_stat, rdev_minor, "u64");
        FIELD(koru_stat, atime_sec, "i64");
        FIELD(koru_stat, atime_nsec, "u64");
        FIELD(koru_stat, mtime_sec, "i64");
        FIELD(koru_stat, mtime_nsec, "u64");
        FIELD(koru_stat, ctime_sec, "i64");
        FIELD(koru_stat, ctime_nsec, "u64");
        FIELD(koru_stat, btime_sec, "i64");
        FIELD(koru_stat, btime_nsec, "u64");
        FIELD(koru_stat, reserved, "u64[12]");
        END_FIELDS(koru_stat);
    }

    // Evaluated vectors, so the const fns and the C macros are diffed too.
    {
        static const uint32_t vectors[][2] = { { 0, 1 }, { 7, 3 }, { 4095, 1 }, { 65535, 65535 } };
        for (const auto &v : vectors) {
            uint32_t h = KORU_MAKE_HANDLE(v[0], v[1]);
            out += fmt("handle %llu %llu %llu %llu %llu\n", (unsigned long long)v[0],
                       (unsigned long long)v[1], (unsigned long long)h,
                       (unsigned long long)KORU_HANDLE_INDEX(h),
                       (unsigned long long)KORU_HANDLE_GEN(h));
            counts.handles++;
        }
    }

    // Closed has no errno preimage, so only this loop emits it.
#define KIND_ROW(name, value)                                                  \
    out += fmt("kind %s %llu\n", #name, (unsigned long long)KORU_KIND_##name); \
    counts.kinds++;
    KORU_KIND_TABLE(KIND_ROW)
#undef KIND_ROW

    // Assert the order rather than sorting: a misplaced row must fail.
    int prev = 0;
#define ERRNO_ROW(name, value, kind)                                                 \
    if ((value) <= prev) {                                                           \
        fprintf(stderr, "abi_dump: %s is out of order or duplicated\n", #name);      \
        exit(1);                                                                     \
    }                                                                                \
    prev = (value);                                                                  \
    out += fmt("errno %s %llu %s %llu\n", #name, (unsigned long long)(value), #kind, \
               (unsigned long long)KORU_KIND_##kind);                                \
    counts.errnos++;
    KORU_ERRNO_TABLE(ERRNO_ROW)
#undef ERRNO_ROW

    out +=
        fmt("counts consts %llu ioctls %llu opcodes %llu structs %llu fields %llu handles %llu "
            "kinds %llu errnos %llu\n",
            counts.consts, counts.ioctls, counts.opcodes, counts.structs, counts.fields,
            counts.handles, counts.kinds, counts.errnos);

    fputs(out.c_str(), stdout);
    return 0;
}
