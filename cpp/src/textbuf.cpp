// SPDX-License-Identifier: MIT

#include <koru/textbuf.hpp>

#include <koru/filebuf.hpp>

namespace koru {

namespace {

/// Rust's `String` knows its own boundaries; here the byte says so.
bool is_boundary(Str s, size_t at)
{
    if (at == 0 || at >= s.size())
        return true;
    return (u8(s[at]) & 0xc0) != 0x80;
}

size_t min_of(size_t a, size_t b)
{
    return a < b ? a : b;
}

} // namespace

// ---------------------------------------------------------------------------
// TextBuf
// ---------------------------------------------------------------------------

void TextBuf::load(Str utf8)
{
    lines_.clear();
    size_t at = 0;
    for (;;) {
        size_t nl = utf8.find('\n', at);
        if (nl == Str::npos) {
            lines_.emplace_back(utf8.substr(at));
            break;
        }
        lines_.emplace_back(utf8.substr(at, nl - at));
        at = nl + 1;
        if (at == utf8.size())
            break;
    }
    if (lines_.empty())
        lines_.emplace_back();
    modified_ = false;
}

String TextBuf::serialize() const
{
    String out;
    for (const String &l : lines_) {
        out += l;
        out += '\n';
    }
    return out;
}

void TextBuf::ensure(size_t row)
{
    while (lines_.size() <= row)
        lines_.emplace_back();
}

size_t TextBuf::clamp(size_t row, size_t at) const
{
    Str s = line(row);
    at    = min_of(at, s.size());
    return is_boundary(s, at) ? at : prev(row, at);
}

void TextBuf::insert(size_t row, size_t at, Str utf8)
{
    ensure(row);
    lines_[row].insert(clamp(row, at), utf8);
    modified_ = true;
}

size_t TextBuf::erase(size_t row, size_t at)
{
    if (row >= lines_.size() || at >= lines_[row].size())
        return 0;
    size_t end = next(row, at);
    lines_[row].erase(at, end - at);
    modified_ = true;
    return end - at;
}

void TextBuf::split(size_t row, size_t at)
{
    ensure(row);
    size_t k    = clamp(row, at);
    String tail = lines_[row].substr(k);
    lines_[row].truncate(k);
    lines_.insert(lines_.begin() + long(row) + 1, std::move(tail));
    modified_ = true;
}

size_t TextBuf::join(size_t row)
{
    if (row + 1 >= lines_.size())
        return npos;
    size_t at = lines_[row].size();
    lines_[row] += lines_[row + 1];
    lines_.erase(lines_.begin() + long(row) + 1);
    modified_ = true;
    return at;
}

size_t TextBuf::prev(size_t row, size_t at) const
{
    Str s = line(row);
    at    = min_of(at, s.size());
    while (at > 0) {
        at--;
        if (is_boundary(s, at))
            break;
    }
    return at;
}

size_t TextBuf::next(size_t row, size_t at) const
{
    Str s = line(row);
    if (at >= s.size())
        return s.size();
    at++;
    while (at < s.size() && !is_boundary(s, at))
        at++;
    return at;
}

size_t TextBuf::column(size_t row, size_t at) const
{
    Str s      = line(row);
    size_t end = min_of(at, s.size());
    size_t n   = 0;
    for (size_t i = 0; i < end; i++)
        if (is_boundary(s, i))
            n++;
    return n;
}

size_t TextBuf::offset(size_t row, size_t col) const
{
    Str s    = line(row);
    size_t n = 0;
    for (size_t i = 0; i < s.size(); i++)
        if (is_boundary(s, i)) {
            if (n == col)
                return i;
            n++;
        }
    return s.size();
}

// ---------------------------------------------------------------------------
// TextView
// ---------------------------------------------------------------------------

void TextView::scroll(i32 delta, size_t lines, u32 height)
{
    size_t most = lines > height ? lines - height : 0;
    if (delta < 0) {
        size_t back = size_t(-i64(delta));
        top_        = top_ > back ? top_ - back : 0;
    } else {
        top_ += size_t(delta);
    }
    top_ = min_of(top_, most);
}

void TextView::to(size_t row, size_t lines, u32 height)
{
    size_t most = lines > height ? lines - height : 0;
    top_        = min_of(row, most);
}

void TextView::follow(size_t row, size_t col, u32 height, u32 width)
{
    if (height == 0 || width == 0)
        return;
    if (row < top_)
        top_ = row;
    else if (row >= top_ + height)
        top_ = row - height + 1;
    if (col < left_)
        left_ = col;
    else if (col >= left_ + width)
        left_ = col - width + 1;
}

void TextView::paint(Pane &p, Grid &g, const TextBuf &b) const
{
    size_t lines = b.lines();
    for (u32 y = 0; y < p.height(); y++) {
        p.move_to(0, y);
        size_t row = top_ + y;
        if (row < lines) {
            Str s     = b.line(row);
            size_t at = min_of(b.offset(row, left_), s.size());
            p.write(g, s.substr(at));
        }
        p.fill_row(g);
    }
}

} // namespace koru
