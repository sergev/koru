// SPDX-License-Identifier: MIT

#include <koru/args.hpp>

namespace koru {

Args Args::from_env(int argc, char **argv)
{
    std::vector<String> v;
    for (int i = 0; i < argc; i++)
        v.push_back(argv[i] ? String(argv[i]) : String());
    return Args(std::move(v));
}

Args Args::skip(size_t n) const
{
    Args out;
    out.v_     = v_;
    size_t at  = start_ + n;
    out.start_ = at < start_ || at > v_->size() ? v_->size() : at; // saturating
    return out;
}

} // namespace koru
