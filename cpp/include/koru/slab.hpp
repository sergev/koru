// SPDX-License-Identifier: MIT
//
// The op slab and its generational key.
//
// Generic in its payload, so the mechanics are testable with no device: the
// production payload owns a `BufSlot` and a coroutine handle, the tests use a
// counter.

#ifndef KORU_SLAB_HPP
#define KORU_SLAB_HPP

#include <cstdint>
#include <utility>
#include <vector>

namespace koru {

/// A `user_data` value: index low, generation high.
///
/// Not the kernel's handle, which is two `uint16_t`. The generation is what
/// stops a late completion for a cancelled op from resuming whatever reused
/// the index — the whole of the answer to the dangling-frame hazard.
struct Cookie {
    uint64_t value = 0;

    constexpr Cookie() = default;
    constexpr explicit Cookie(uint64_t v) : value(v) {}
    constexpr Cookie(uint32_t index, uint32_t generation)
        : value(uint64_t(index) | (uint64_t(generation) << 32))
    {
    }

    constexpr uint32_t index() const { return uint32_t(value); }
    constexpr uint32_t generation() const { return uint32_t(value >> 32); }

    friend constexpr bool operator==(Cookie a, Cookie b) { return a.value == b.value; }
    friend constexpr bool operator!=(Cookie a, Cookie b) { return a.value != b.value; }
};

/// A slab keyed by [`Cookie`]. Indices are recycled, generations are not.
template <class T>
class Slab {
public:
    /// The cookie this value can be reached by until it is removed.
    Cookie insert(T value)
    {
        live_++;
        if (!free_.empty()) {
            uint32_t index = free_.back();
            free_.pop_back();
            Entry &e = entries_[index];
            e.live   = true;
            e.value  = std::move(value);
            return Cookie(index, e.generation);
        }
        // Generations start at 1, so a valid cookie is never 0.
        entries_.push_back(Entry{ 1, true, std::move(value) });
        return Cookie(uint32_t(entries_.size() - 1), 1);
    }

    /// The value, or nullptr where the cookie is stale or was never live.
    T *get(Cookie c)
    {
        if (c.index() >= entries_.size())
            return nullptr;
        Entry &e = entries_[c.index()];
        if (!e.live || e.generation != c.generation())
            return nullptr;
        return &e.value;
    }

    const T *get(Cookie c) const { return const_cast<Slab *>(this)->get(c); }

    /// Frees the entry and bumps its generation, so every cookie naming it is
    /// stale from here on. False where it was not live.
    bool remove(Cookie c)
    {
        if (!get(c))
            return false;
        Entry &e     = entries_[c.index()];
        e.live       = false;
        e.value      = T();
        e.generation = bump(e.generation);
        free_.push_back(c.index());
        live_--;
        return true;
    }

    size_t live() const { return live_; }
    size_t capacity() const { return entries_.size(); }

private:
    /// The generation after `g`, skipping 0 so no live cookie is ever zero.
    static uint32_t bump(uint32_t g) { return g + 1 == 0 ? 1 : g + 1; }

    struct Entry {
        uint32_t generation = 1;
        bool live           = false;
        T value;
    };

    std::vector<Entry> entries_;
    std::vector<uint32_t> free_;
    size_t live_ = 0;
};

} // namespace koru

#endif // KORU_SLAB_HPP
