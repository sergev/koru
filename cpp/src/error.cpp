// SPDX-License-Identifier: MIT

#include <koru/error.hpp>

#include <cstring>

namespace koru {

const ErrnoDef KORU_ERRNOS[] = {
#define KORU_ERRNO_ROW(name, value, kind) { Errno(value), #name, Kind::kind },
    KORU_ERRNO_TABLE(KORU_ERRNO_ROW)
#undef KORU_ERRNO_ROW
};

size_t koru_errnos_len()
{
    return sizeof(KORU_ERRNOS) / sizeof(KORU_ERRNOS[0]);
}

const Kind KINDS[15] = {
#define KORU_KIND_ROW(name, value) Kind::name,
    KORU_KIND_TABLE(KORU_KIND_ROW)
#undef KORU_KIND_ROW
};

const char *Errno::name() const
{
    for (size_t i = 0; i < koru_errnos_len(); i++)
        if (KORU_ERRNOS[i].err == *this)
            return KORU_ERRNOS[i].name;
    return nullptr;
}

const char *Errno::message() const
{
    return strerror(value);
}

Kind kind_of(Errno e)
{
    for (size_t i = 0; i < koru_errnos_len(); i++)
        if (KORU_ERRNOS[i].err == e)
            return KORU_ERRNOS[i].kind;
    return Kind::Io;
}

const char *kind_name(Kind k)
{
    switch (k) {
    case Kind::Invalid:
        return "invalid";
    case Kind::NoMemory:
        return "out of memory";
    case Kind::NotFound:
        return "not found";
    case Kind::Exists:
        return "already exists";
    case Kind::NotDir:
        return "not a directory";
    case Kind::IsDir:
        return "is a directory";
    case Kind::Perm:
        return "permission denied";
    case Kind::Io:
        return "i/o error";
    case Kind::Cancelled:
        return "cancelled";
    case Kind::Again:
        return "try again";
    case Kind::Unsupported:
        return "unsupported";
    case Kind::Closed:
        return "closed";
    case Kind::NotEmpty:
        return "directory not empty";
    case Kind::Loop:
        return "too many symbolic links";
    case Kind::Intr:
        return "interrupted";
    }
    return "unknown";
}

const char *Error::message() const
{
    return raw_.value == 0 ? kind_name(kind_) : raw_.message();
}

result<uint64_t, Errno> from_res(int64_t res)
{
    if (res >= 0)
        return result<uint64_t, Errno>(uint64_t(res));
    return result<uint64_t, Errno>(Errno(int(-res)));
}

int64_t to_res(result<uint64_t, Errno> r)
{
    return r.ok() ? int64_t(r.value()) : -int64_t(r.error().value);
}

} // namespace koru
