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

} // namespace koru
