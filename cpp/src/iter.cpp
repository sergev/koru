// SPDX-License-Identifier: MIT

#include <koru/iter.hpp>

namespace koru {

// ---------------------------------------------------------------------------
// Input
// ---------------------------------------------------------------------------

Input::Input(Args paths, Handle fallback, Str who)
    : paths_(std::move(paths)), who_(who), fd_(fallback)
{
    own_ = !paths_.empty();
}

task<result<String>> Input::read()
{
    if (!own_)
        co_return co_await read_chunk(fd_);

    for (;;) {
        if (cur_ == 0) {
            if (at_ >= paths_.size())
                co_return Error::closed();
            String path       = String(paths_[at_]);
            result<Handle> h = co_await open_read(path);
            if (!h.ok()) {
                // Reported here: the caller has no name for the file, and maps
                // anything but Closed onto an exit status without printing.
                co_await errln(who_, path, h.error());
                at_ = paths_.size(); // spent, not to be retried
                co_return h.error();
            }
            cur_ = h.value();
        }

        result<String> chunk = co_await read_chunk(cur_);
        if (chunk.ok() || !(chunk.error() == Kind::Closed))
            co_return std::move(chunk);

        // Closed before the next is opened, which is what keeps one open.
        co_await close_fd(cur_);
        cur_ = 0;
        at_++;
    }
}

// ---------------------------------------------------------------------------
// LineReader
// ---------------------------------------------------------------------------

bool LineReader::take(String &out)
{
    size_t nl = buf_.find('\n', pos_);
    if (nl != String::npos) {
        out.assign(buf_, pos_, nl - pos_);
        pos_ = nl + 1;
        if (pos_ == buf_.size()) {
            buf_.clear();
            pos_ = 0;
        }
        return true;
    }
    if (!eof_ || pos_ == buf_.size())
        return false;
    // A final fragment with no newline is a line.
    out.assign(buf_, pos_, String::npos);
    buf_.clear();
    pos_ = 0;
    return true;
}

task<result<bool>> LineReader::next(String &out)
{
    for (;;) {
        // The fast half: no syscall where the bytes in hand answer.
        if (take(out))
            co_return true;
        if (eof_) {
            out.clear();
            co_return false;
        }

        result<String> chunk = co_await src_.read();
        if (chunk.ok()) {
            // The unread tail slides down before the buffer takes more, so a
            // long-running reader does not grow it without bound.
            if (pos_ > 0) {
                buf_.erase(0, pos_);
                pos_ = 0;
            }
            buf_ += chunk.value();
        } else if (chunk.error() == Kind::Closed) {
            eof_ = true;
        } else {
            co_return chunk.error();
        }
    }
}

// ---------------------------------------------------------------------------
// TreeWalk
// ---------------------------------------------------------------------------

TreeWalk::TreeWalk(Str root)
{
    // One trailing slash or several: what is reported is the trimmed root, a
    // '/', and the rest, so root_len splices a destination on.
    Str r = root;
    while (r.size() > 1 && r.back() == '/')
        r.remove_suffix(1);
    root_ = String(r);
}

bool TreeWalk::report(String &path, DirEntry &out)
{
    while (!levels_.empty() && levels_.back().at == levels_.back().ents.size())
        levels_.pop_back();
    if (levels_.empty())
        return false;

    Level &lv = levels_.back();
    DirEntry e = lv.ents[lv.at];
    lv.at++;

    path = lv.path;
    if (path.empty() || path.back() != '/')
        path += '/';
    path += e.name;

    // Descended into on the next call, so the caller sees a directory before
    // what is in it.
    if (e.kind == FileKind::Dir)
        pending_ = path;
    out = std::move(e);
    return true;
}

task<result<bool>> TreeWalk::next(String &path, DirEntry &e)
{
    at_.clear();

    // The fast half: an entry this walk already holds needs no listing.
    if (began_ && !pending_)
        co_return report(path, e);

    // The root the first time, then whichever directory was reported last: one
    // listing per call at most, so a failure names one directory.
    String dir = pending_ ? *pending_ : root_;
    pending_.reset();
    began_ = true;

    result<std::vector<DirEntry>> ents = co_await list_dir(dir);
    if (!ents.ok()) {
        at_ = dir;
        co_return ents.error();
    }
    Level lv;
    lv.ents = std::move(ents).take();
    lv.at   = 0;
    lv.path = std::move(dir);
    levels_.push_back(std::move(lv));
    co_return report(path, e);
}

} // namespace koru
