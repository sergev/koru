// SPDX-License-Identifier: MIT
//
// A one-translation-unit probe: koru_abi.h must compile as C11 under
// -Wall -Wextra -Werror -Wpedantic, not only as C++. test/ proves the same
// thing, but only after a VM build; this proves it from `cmake --build`.

#include <koru_abi.h>

// Not an empty translation unit, which -Wpedantic rejects.
const char koru_abi_c11_tu[] = KORU_DEV;
