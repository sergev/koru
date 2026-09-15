// SPDX-License-Identifier: MIT
//
// Braam's `kernel/alloc.h`, the two names a program uses of it. There they are
// the kernel heap's typed allocation, checking for null because `-fno-
// exceptions` makes plain `new` construct at address zero; here the C++
// allocator throws instead and `new (std::nothrow)` is the same promise.
//
// Nothing else of that header is here: `heap_stats`, `heap_origin` and the
// size-class arithmetic describe an allocator koru does not have.

#ifndef KORU_ALLOC_HPP
#define KORU_ALLOC_HPP

#include <new>
#include <utility>

namespace koru {

template <class T, class... A>
T *heap_new(A &&...a)
{
    return new (std::nothrow) T(std::forward<A>(a)...);
}

template <class T>
void heap_delete(T *p)
{
    delete p;
}

} // namespace koru

#endif // KORU_ALLOC_HPP
