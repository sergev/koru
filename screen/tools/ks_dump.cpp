// SPDX-License-Identifier: MIT
//
// The canonical screen-protocol dump, C++ side. Must be byte-identical to
// rust/runtime/src/bin/ks_dump.rs; scripts/abi.sh diffs the two. The grammar is
// T14's, with two record kinds of its own: `opvec`, which evaluates the
// validation helpers over every op number, and `style`, which evaluates the
// pack and its three accessors.

#include <ks_abi.h>

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <string>

// Bumped when the grammar changes, which is an edit to both emitters.
#define DUMP_VERSION 1

namespace {

std::string out;

struct Counts {
    unsigned long long consts = 0;
    unsigned long long ops    = 0;
    unsigned long long opvecs = 0;
    unsigned long long styles = 0;
    unsigned long long structs = 0;
    unsigned long long fields  = 0;
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
        fprintf(stderr, "ks_dump: record does not fit\n");
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
        fprintf(stderr, "ks_dump: %s: field sizes sum to %llu, sizeof is %llu\n", name, sum, size);
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

#define END_FIELDS(s) emit_struct(#s, sizeof(struct s), KS_ALIGNOF(struct s), body, n, sum)

int main()
{
    out += fmt("ks-dump %llu\n", (unsigned long long)DUMP_VERSION);
    out += fmt("sock %s\n", KS_SOCK_NAME);

    konst("KS_MAGIC", "u32", KS_MAGIC);
    konst("KS_ABI_VERSION", "u32", KS_ABI_VERSION);
    konst("KS_ALIGN", "u32", KS_ALIGN);
    konst("KS_SEQ_UNSOLICITED", "u32", KS_SEQ_UNSOLICITED);
    konst("KS_MAX_FRAME", "u32", KS_MAX_FRAME);
    konst("KS_OP_MAX", "u32", KS_OP_MAX);
    konst("KS_F_TAKE", "u16", KS_F_TAKE);
    konst("KS_F_SET", "u16", KS_F_SET);
    konst("KS_F_STALE", "u16", KS_F_STALE);
    konst("KS_F_ECHO_SHOW", "u16", KS_F_ECHO_SHOW);
    konst("KS_F_ECHO_FRESH", "u16", KS_F_ECHO_FRESH);
    konst("KS_F_ECHO_END", "u16", KS_F_ECHO_END);
    konst("KS_LEN_VARIABLE", "u32", KS_LEN_VARIABLE);
    konst("KS_LEN_NONE", "u32", KS_LEN_NONE);
    konst("KS_TTY_CONSOLE", "u32", KS_TTY_CONSOLE);
    konst("KS_ECHO_RUNS_MAX", "u32", KS_ECHO_RUNS_MAX);
    konst("KS_ATTR_BOLD", "u8", KS_ATTR_BOLD);
    konst("KS_ATTR_UNDERLINE", "u8", KS_ATTR_UNDERLINE);
    konst("KS_ATTR_REVERSE", "u8", KS_ATTR_REVERSE);
    konst("KS_ATTRS_ALL", "u8", KS_ATTRS_ALL);
    konst("KS_COLOR_BLACK", "u8", KS_COLOR_BLACK);
    konst("KS_COLOR_RED", "u8", KS_COLOR_RED);
    konst("KS_COLOR_GREEN", "u8", KS_COLOR_GREEN);
    konst("KS_COLOR_YELLOW", "u8", KS_COLOR_YELLOW);
    konst("KS_COLOR_BLUE", "u8", KS_COLOR_BLUE);
    konst("KS_COLOR_MAGENTA", "u8", KS_COLOR_MAGENTA);
    konst("KS_COLOR_CYAN", "u8", KS_COLOR_CYAN);
    konst("KS_COLOR_WHITE", "u8", KS_COLOR_WHITE);
    konst("KS_COLOR_BRIGHT", "u8", KS_COLOR_BRIGHT);
    konst("KS_COLORS", "u32", KS_COLORS);
    konst("KS_MAX_COLS", "u32", KS_MAX_COLS);
    konst("KS_MAX_ROWS", "u32", KS_MAX_ROWS);
    konst("KS_PAGE_SIZE", "u32", KS_PAGE_SIZE);
    konst("KS_MIN_SLOT", "u32", KS_MIN_SLOT);
    konst("KS_STYLE_KEEP", "u32", KS_STYLE_KEEP);
    konst("KS_MOD_SHIFT", "u32", KS_MOD_SHIFT);
    konst("KS_MOD_CTRL", "u32", KS_MOD_CTRL);
    konst("KS_MOD_ALT", "u32", KS_MOD_ALT);
    konst("KS_MOD_META", "u32", KS_MOD_META);
    konst("KS_MODS_ALL", "u32", KS_MODS_ALL);
    konst("KS_KEY_NAMED", "u32", KS_KEY_NAMED);
    konst("KS_KEY_MAX", "u32", KS_KEY_MAX);
    konst("KS_EINTR", "i32", KS_EINTR);
    konst("KS_EBUSY", "i32", KS_EBUSY);
    konst("KS_EINVAL", "i32", KS_EINVAL);
    konst("KS_ENOTTY", "i32", KS_ENOTTY);
    konst("KS_EPROTO", "i32", KS_EPROTO);
    konst("KS_ENOSYS", "i32", KS_ENOSYS);
    konst("KS_EPERM", "i32", KS_EPERM);

#define OP(name)                                                         \
    do {                                                                 \
        out += fmt("op %s %llu\n", #name, (unsigned long long)name);     \
        counts.ops++;                                                    \
    } while (0)
    OP(KS_OP_HELLO);
    OP(KS_OP_KEY_CLAIM);
    OP(KS_OP_KEY_READ);
    OP(KS_OP_SCREEN_CLAIM);
    OP(KS_OP_BLIT);
    OP(KS_OP_CURSOR);
    OP(KS_OP_ECHO);
    OP(KS_OP_STYLE);
    OP(KS_OP_SCREEN_CLEAR);
    OP(KS_OP_TTY);
    OP(KS_OP_TERM_OPEN);
#undef OP

    // The named keys, every one, because a client that renumbers them draws
    // the wrong thing rather than failing.
#define KEY(name)                                                        \
    do {                                                                 \
        out += fmt("key %s %llu\n", #name, (unsigned long long)name);    \
        counts.ops++;                                                    \
    } while (0)
    KEY(KS_KEY_ENTER);
    KEY(KS_KEY_BACKSPACE);
    KEY(KS_KEY_TAB);
    KEY(KS_KEY_ESCAPE);
    KEY(KS_KEY_DELETE);
    KEY(KS_KEY_INSERT);
    KEY(KS_KEY_UP);
    KEY(KS_KEY_DOWN);
    KEY(KS_KEY_LEFT);
    KEY(KS_KEY_RIGHT);
    KEY(KS_KEY_HOME);
    KEY(KS_KEY_END);
    KEY(KS_KEY_PAGE_UP);
    KEY(KS_KEY_PAGE_DOWN);
    KEY(KS_KEY_F1);
    KEY(KS_KEY_F2);
    KEY(KS_KEY_F3);
    KEY(KS_KEY_F4);
    KEY(KS_KEY_F5);
    KEY(KS_KEY_F6);
    KEY(KS_KEY_F7);
    KEY(KS_KEY_F8);
    KEY(KS_KEY_F9);
    KEY(KS_KEY_F10);
    KEY(KS_KEY_F11);
    KEY(KS_KEY_F12);
#undef KEY

    {
        BEGIN_FIELDS();
        FIELD(ks_head, len, "u32");
        FIELD(ks_head, op, "u16");
        FIELD(ks_head, flags, "u16");
        FIELD(ks_head, seq, "u32");
        FIELD(ks_head, res, "i32");
        END_FIELDS(ks_head);
    }
    {
        BEGIN_FIELDS();
        FIELD(ks_hello, magic, "u32");
        FIELD(ks_hello, version, "u32");
        END_FIELDS(ks_hello);
    }
    {
        BEGIN_FIELDS();
        FIELD(ks_hello_rep, magic, "u32");
        FIELD(ks_hello_rep, version, "u32");
        FIELD(ks_hello_rep, cols, "u32");
        FIELD(ks_hello_rep, rows, "u32");
        FIELD(ks_hello_rep, max_cols, "u32");
        FIELD(ks_hello_rep, max_rows, "u32");
        FIELD(ks_hello_rep, max_frame, "u32");
        FIELD(ks_hello_rep, features, "u32");
        END_FIELDS(ks_hello_rep);
    }
    {
        BEGIN_FIELDS();
        FIELD(ks_geom, cols, "u32");
        FIELD(ks_geom, rows, "u32");
        END_FIELDS(ks_geom);
    }
    {
        BEGIN_FIELDS();
        FIELD(ks_key, code, "u32");
        FIELD(ks_key, mods, "u32");
        FIELD(ks_key, cols, "u32");
        FIELD(ks_key, rows, "u32");
        END_FIELDS(ks_key);
    }
    {
        BEGIN_FIELDS();
        FIELD(ks_blit, x, "u32");
        FIELD(ks_blit, y, "u32");
        FIELD(ks_blit, w, "u32");
        FIELD(ks_blit, h, "u32");
        FIELD(ks_blit, cursor_x, "u32");
        FIELD(ks_blit, cursor_y, "u32");
        FIELD(ks_blit, cursor_on, "u32");
        FIELD(ks_blit, cols, "u32");
        FIELD(ks_blit, rows, "u32");
        FIELD(ks_blit, rsvd0, "u32");
        END_FIELDS(ks_blit);
    }
    {
        BEGIN_FIELDS();
        FIELD(ks_cell, ch, "u32");
        FIELD(ks_cell, fg, "u8");
        FIELD(ks_cell, bg, "u8");
        FIELD(ks_cell, attrs, "u8");
        FIELD(ks_cell, rsvd0, "u8");
        END_FIELDS(ks_cell);
    }
    {
        BEGIN_FIELDS();
        FIELD(ks_cursor_req, x, "u32");
        FIELD(ks_cursor_req, y, "u32");
        FIELD(ks_cursor_req, on, "u32");
        FIELD(ks_cursor_req, rsvd0, "u32");
        END_FIELDS(ks_cursor_req);
    }
    {
        BEGIN_FIELDS();
        FIELD(ks_cursor, x, "u32");
        FIELD(ks_cursor, y, "u32");
        FIELD(ks_cursor, on, "u32");
        FIELD(ks_cursor, cols, "u32");
        FIELD(ks_cursor, rows, "u32");
        FIELD(ks_cursor, scrolled, "u32");
        END_FIELDS(ks_cursor);
    }
    {
        BEGIN_FIELDS();
        FIELD(ks_echo, x, "u32");
        FIELD(ks_echo, y, "u32");
        FIELD(ks_echo, cur, "u32");
        FIELD(ks_echo, runs, "u32");
        END_FIELDS(ks_echo);
    }
    {
        BEGIN_FIELDS();
        FIELD(ks_run, style, "u32");
        FIELD(ks_run, len, "u32");
        END_FIELDS(ks_run);
    }
    {
        BEGIN_FIELDS();
        FIELD(ks_style, style, "u32");
        FIELD(ks_style, rsvd0, "u32");
        END_FIELDS(ks_style);
    }
    {
        BEGIN_FIELDS();
        FIELD(ks_tty, flags, "u32");
        FIELD(ks_tty, cols, "u32");
        FIELD(ks_tty, rows, "u32");
        FIELD(ks_tty, rsvd0, "u32");
        END_FIELDS(ks_tty);
    }

    // Evaluated over every op number and one past the end, so the validation
    // helpers are diffed rather than the constants they are built from. An op
    // the daemon does not know must answer no flags and no length on both
    // sides, which is what makes "unknown op" one answer and not two.
    for (uint32_t op = 0; op <= KS_OP_MAX; op++) {
        out += fmt("opvec %llu flags %llu req %llu rep %llu err %llu eintr %llu\n",
                   (unsigned long long)op, (unsigned long long)ks_flags_all(op),
                   (unsigned long long)ks_req_len(op), (unsigned long long)ks_rep_len(op),
                   (unsigned long long)ks_err_len(op, -KS_EINVAL),
                   (unsigned long long)ks_err_len(op, -KS_EINTR));
        counts.opvecs++;
    }

    {
        static const unsigned char vectors[][3] = {
            { 0, 0, 0 },
            { KS_COLOR_RED, KS_COLOR_BLACK, KS_ATTR_BOLD },
            { KS_COLOR_WHITE + KS_COLOR_BRIGHT, KS_COLOR_BLUE, KS_ATTRS_ALL },
            { 255, 255, 255 },
        };
        for (const auto &v : vectors) {
            uint32_t s = ks_style_pack(v[0], v[1], v[2]);
            out += fmt("style %llu %llu %llu packed %llu fg %llu bg %llu attrs %llu\n",
                       (unsigned long long)v[0], (unsigned long long)v[1],
                       (unsigned long long)v[2], (unsigned long long)s,
                       (unsigned long long)ks_style_fg(s), (unsigned long long)ks_style_bg(s),
                       (unsigned long long)ks_style_attrs(s));
            counts.styles++;
        }
    }

    out += fmt("counts consts %llu ops %llu opvecs %llu styles %llu structs %llu fields %llu\n",
               counts.consts, counts.ops, counts.opvecs, counts.styles, counts.structs,
               counts.fields);

    fputs(out.c_str(), stdout);
    return 0;
}
