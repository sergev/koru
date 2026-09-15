// SPDX-License-Identifier: MIT
//
// Braam's surface at global scope. Include this and nothing else, and a source
// written against Braam's `doc/Programming_Manual.md` compiles: no `koru::`,
// no `std::`, no namespace qualification anywhere.
//
// That is the whole of the C++ binding's portability claim, and T49 is what
// tests it — with the include line as the only edit to a real program.
//
// Everything here is a `using` declaration. There is no second definition of
// anything: `Task` is `koru::task`, `Result` is `koru::result`, and `Error` is
// the one libkoru has had since T39.

#ifndef KORU_BRAAM_HPP
#define KORU_BRAAM_HPP

#include <koru/alloc.hpp>
#include <koru/args.hpp>
#include <koru/file.hpp>
#include <koru/fmt.hpp>
#include <koru/ftoa.hpp>
#include <koru/iter.hpp>
#include <koru/ops.hpp>
#include <koru/opt.hpp>
#include <koru/path.hpp>
#include <koru/rt.hpp>
#include <koru/screen.hpp>
#include <koru/size.hpp>
#include <koru/sysabi.hpp>
#include <koru/task.hpp>
#include <koru/text.hpp>
#include <koru/textbuf.hpp>
#include <koru/time.hpp>
#include <koru/usage.hpp>
#include <koru/vocab.hpp>

// --------------------------------------------------------------- the grammar

/// Braam's `Task<T>`: lazy, move-only, transferring symmetrically.
template <class T = void>
using Task = koru::task<T>;

/// Braam's `Result<T>`. `TRY`, `TRY_VOID`, `CO_TRY` and `CO_TRY_VOID` are
/// macros and are already global.
template <class T, class E = koru::Error>
using Result = koru::result<T, E>;

using koru::None;
using koru::Option;
using koru::Span;
using koru::SpanMut;
using koru::Str;
using koru::String;
using koru::Vec;

/// Braam's `kernel/traits.h`: the slice of `<utility>` a program uses, at
/// global scope because there is no `std::` in that tree. These are `std::`'s
/// own — a second definition beside them would be ambiguous by ADL, because
/// `String` derives from `std::string` and so makes `std` an associated
/// namespace. What that costs is one clang warning; CMakeLists.txt says which.
using std::forward;
using std::max;
using std::min;
using std::move;
using std::swap;

using koru::heap_delete;
using koru::heap_new;

using koru::f64;
using koru::i16;
using koru::i32;
using koru::i64;
using koru::i8;
using koru::u16;
using koru::u32;
using koru::u64;
using koru::u8;
using koru::isize;
using koru::usize;

using koru::Err;
using koru::Errno;
using koru::Error;
using koru::error_name;
using koru::Kind;
using koru::kind_name;

// ------------------------------------------------------------- the runtime

using koru::Args;
using koru::at_exit;
using koru::block_on;
using koru::err_fd;
using koru::in_fd;
using koru::out_fd;
using koru::proc_pid;
using koru::spawn;

// ------------------------------------------------------------ the operations

using koru::SYS_CHUNK;
using koru::SYS_KIND_DIR;
using koru::SYS_KIND_FILE;
using koru::SYS_KIND_LINK;
using koru::SYS_O_ALL;
using koru::SYS_O_APPEND;
using koru::SYS_O_CREATE;
using koru::SYS_O_EXCL;
using koru::SYS_O_READ;
using koru::SYS_O_TRUNC;
using koru::SYS_O_WRITE;
using koru::SYS_READ_MAX;
using koru::SYS_SEEK_CUR;
using koru::SYS_SEEK_END;
using koru::SYS_SEEK_MAX;
using koru::SYS_SEEK_SET;
using koru::SYS_STDERR;
using koru::SYS_STDIN;
using koru::SYS_STDOUT;

using koru::Clock;
using koru::DirEntry;
using koru::FileInfo;
using koru::FileKind;
using koru::Handle;

using koru::CHUNK;
using koru::CREATE_MODE;
using koru::O_ALL;
using koru::O_APPEND;
using koru::O_CREATE;
using koru::O_EXCL;
using koru::O_READ;
using koru::O_TRUNC;
using koru::O_WRITE;
using koru::READ_MAX;
using koru::SEEK_CUR;
using koru::SEEK_END;
using koru::SEEK_MAX;
using koru::SEEK_SET;

using koru::clock_now;
using koru::close_fd;
using koru::copy_file;
using koru::copy_tree;
using koru::cwd_get;
using koru::cwd_set;
using koru::dup_fd;
using koru::errln;
using koru::next_field;
using koru::next_line;
using koru::list_dir;
using koru::make_dir;
using koru::make_dir_all;
using koru::make_link;
using koru::open_at;
using koru::open_read;
using koru::read_chunk;
using koru::read_file;
using koru::read_link;
using koru::read_some;
using koru::remove_path;
using koru::rename_path;
using koru::seek_fd;
using koru::sleep_for;
using koru::stat_fd;
using koru::stat_of;
using koru::touch_path;
using koru::truncate_fd;
using koru::write_all;

// ---------------------------------------------------------------- the stream

using koru::Buffering;
using koru::File;
using koru::FileMode;
using koru::FILE_BUF;
using koru::RUNE_REPLACEMENT;
using koru::utf8_decode;
using koru::utf8_encode;
using koru::utf8_rune;

using koru::get_rune;
using koru::put_rune;
using koru::write_err;
using koru::write_out;

using koru::Input;
using koru::LineReader;
using koru::TreeWalk;

// ------------------------------------------------------ the pure libraries
//
// `kernel/fmt.h`, `kernel/text.h`, `fs/path.h` and `proc/size.h`. Not one of
// them touches the ring; every one of them is what a `src/cmd` source reaches
// for without thinking about it.

using koru::Buf;

// `math/math.h`'s classifiers. The rest of libm is already at global scope
// through `<cmath>`; these five are macros in C and only `std::` has them.
using std::isfinite;
using std::isinf;
using std::isnan;
using std::isnormal;
using std::signbit;

using koru::fmt_f64;
using koru::fmt_f64_padded;
using koru::fmt_f64_shortest;
using koru::parse_f64;
using koru::put_f64;
using koru::scan_f32;
using koru::scan_f64;

using koru::is_digit;
using koru::is_space;
using koru::parse_u32;
using koru::rune_lower;
using koru::rune_safe;
using koru::rune_upper;
using koru::scan_i64;
using koru::scan_space;
using koru::scan_token;
using koru::scan_u64;
using koru::scan_until;

using koru::path_basename;
using koru::path_dirname;
using koru::path_join;
using koru::path_resolve;
using koru::path_under;

using koru::parse_size;
using koru::SIZE_BLOCK;
using koru::SizeMod;
using koru::SizeSpec;
using koru::size_apply;

// ----------------------------------------------------------- the program shell

using koru::help_asked;
using koru::Opt;
using koru::OptError;
using koru::OptParse;
using koru::Opts;
using koru::usage_asked;
using koru::usage_error;

using koru::Civil;
using koru::civil;
using koru::civil_secs;
using koru::TIME_DAYS;
using koru::TIME_MONTHS;

// -------------------------------------------------------------- the screen

using koru::Grid;
using koru::Rect;
using koru::Screen;
using koru::TextBuf;
using koru::TextView;

/// Braam's `Pane` carries the grid it paints on, so `bar.write(line)` is one
/// argument there and two here. This is the one that carries it; koru's
/// unbound `Pane` is `koru::Pane` and a `src/cmd` source never names it.
using Pane = koru::GridPane;

using koru::Geometry;
using koru::ProcScreen;
using koru::TtyInfo;
using koru::tty_of;

using koru::Key;
using koru::Keys;
using koru::Painter;

using koru::ATTR_BOLD;
using koru::ATTR_REVERSE;
using koru::ATTR_UNDERLINE;
using koru::COLOR_BLACK;
using koru::COLOR_BLUE;
using koru::COLOR_BRIGHT;
using koru::COLOR_CYAN;
using koru::COLOR_GREEN;
using koru::COLOR_MAGENTA;
using koru::COLOR_RED;
using koru::COLOR_WHITE;
using koru::COLOR_YELLOW;

using koru::KEY_BACKSPACE;
using koru::KEY_DELETE;
using koru::KEY_DOWN;
using koru::KEY_END;
using koru::KEY_ENTER;
using koru::KEY_ESCAPE;
using koru::KEY_HOME;
using koru::KEY_INSERT;
using koru::KEY_LEFT;
using koru::KEY_NAMED;
using koru::KEY_PAGE_DOWN;
using koru::KEY_PAGE_UP;
using koru::KEY_RIGHT;
using koru::KEY_TAB;
using koru::KEY_UP;
using koru::MOD_ALT;
using koru::MOD_CTRL;
using koru::MOD_META;
using koru::MOD_SHIFT;

// ----------------------------------------------------------------- the entry
//
// Braam's. `koru_main` is libkoru's own and carries a `Result`, because
// `CO_TRY` must have somewhere to send an error; a `src/cmd` source writes
// this one and links `koru_start_proc`, which adapts it.

Task<i32> proc_main(Args args);

#endif // KORU_BRAAM_HPP
