// SPDX-License-Identifier: MIT
//
// Braam's `Args`: the argument vector, `name` first.
//
// Braam's is a `Span<const Str>` in a public member `v`, and its `tail` is a
// subspan, so it costs nothing. This one owns the strings — a C++ program's
// `argv` outlives it, but a test's does not — so `v` is a counted view of a
// shared vector rather than a raw span. It keeps the name and the one method a
// program uses of it, `subspan`, because `Args{ args.v.subspan(n) }` is how
// `head`, `tail` and `grep` hand the rest of a command line on.

#ifndef KORU_ARGS_HPP
#define KORU_ARGS_HPP

#include <koru/vocab.hpp>

#include <memory>
#include <vector>

namespace koru {

class Args {
public:
    /// What Braam spells `Span<const Str>`.
    class Words {
    public:
        Words() : v_(std::make_shared<std::vector<String>>()) {}
        explicit Words(std::vector<String> v)
            : v_(std::make_shared<std::vector<String>>(std::move(v)))
        {
        }

        size_t size() const { return v_->size() - start_; }
        bool empty() const { return size() == 0; }
        Str operator[](size_t i) const { return (*v_)[start_ + i]; }

        /// Saturating, as `Span::subspan` is.
        Words subspan(size_t n) const
        {
            Words out;
            out.v_    = v_;
            size_t at = start_ + n;
            out.start_ = at < start_ || at > v_->size() ? v_->size() : at;
            return out;
        }

        const String *begin() const { return v_->data() + start_; }
        const String *end() const { return v_->data() + v_->size(); }

    private:
        std::shared_ptr<std::vector<String>> v_;
        size_t start_ = 0;
    };

    Args() = default;
    Args(Words w) : v(std::move(w)) {}
    explicit Args(std::vector<String> v) : v(Words(std::move(v))) {}

    /// The process's own.
    static Args from_env(int argc, char **argv);

    Words v;

    size_t size() const { return v.size(); }
    bool empty() const { return v.empty(); }

    /// Argument 0, the program's own name, or empty where there is none.
    Str name() const { return empty() ? Str() : v[0]; }

    /// Everything but the first, sharing the same vector.
    Args tail() const { return skip(1); }

    /// Everything from `n` on, which is what `OptParse::rest` hands back.
    Args skip(size_t n) const { return Args(v.subspan(n)); }

    Str operator[](size_t i) const { return v[i]; }

    const String *begin() const { return v.begin(); }
    const String *end() const { return v.end(); }
};

} // namespace koru

#endif // KORU_ARGS_HPP
