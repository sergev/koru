// SPDX-License-Identifier: MIT
//
// A one-translation-unit probe: ks_abi.h must compile as C11 under
// -Wall -Wextra -Werror -Wpedantic, not only as C++. The daemon is C++ and the
// client is Rust, so without this nothing would ever compile it as C.

#include <ks_abi.h>

// Not an empty translation unit, which -Wpedantic rejects.
const char ks_abi_c11_tu[] = KS_SOCK_NAME;

// The inline functions are only instantiated where they are used.
uint32_t ks_abi_c11_probe(uint32_t op);

uint32_t ks_abi_c11_probe(uint32_t op)
{
    return ks_flags_all(op) + ks_req_len(op) + ks_rep_len(op) + ks_err_len(op, -KS_EINTR) +
           ks_style_pack(KS_COLOR_RED, KS_COLOR_BLACK, KS_ATTR_BOLD) +
           ks_style_fg(KS_STYLE_KEEP) + ks_style_bg(KS_STYLE_KEEP) + ks_style_attrs(KS_STYLE_KEEP);
}
