// SPDX-License-Identifier: MIT
//
// Braam's `Args`: the argument vector, `name` first.
//
// Braam's is a `Span<const Str>` and its `tail` is a subspan, so it costs
// nothing. This one owns the strings — a C++ program's `argv` outlives it, but
// a test's does not — so the vector is shared and `tail` moves a start index.

#ifndef KORU_ARGS_HPP
#define KORU_ARGS_HPP

#include <koru/vocab.hpp>

#include <memory>
#include <vector>

namespace koru {

class Args {
public:
    Args() : v_(std::make_shared<std::vector<String>>()) {}
    explicit Args(std::vector<String> v) : v_(std::make_shared<std::vector<String>>(std::move(v)))
    {
    }

    /// The process's own.
    static Args from_env(int argc, char **argv);

    size_t size() const { return v_->size() - start_; }
    bool empty() const { return size() == 0; }

    /// Argument 0, the program's own name, or empty where there is none.
    Str name() const { return empty() ? Str() : (*this)[0]; }

    /// Everything but the first, sharing the same vector.
    Args tail() const { return skip(1); }

    /// Everything from `n` on, which is what `OptParse::rest` hands back.
    Args skip(size_t n) const;

    Str operator[](size_t i) const { return (*v_)[start_ + i]; }

    const String *begin() const { return v_->data() + start_; }
    const String *end() const { return v_->data() + v_->size(); }

private:
    std::shared_ptr<std::vector<String>> v_;
    size_t start_ = 0;
};

} // namespace koru

#endif // KORU_ARGS_HPP
