// SPDX-License-Identifier: MIT
//
// A free list over the arena's slots.
//
// Advisory only. Exclusivity is kernel-enforced by the `slot_busy` bitmap, and
// a loser gets `-EBUSY` as a completion — which is what makes this safe to get
// wrong in a language that cannot prove a slot was moved rather than copied.
//
// `BufSlot` is move-only, with copy deleted. Use after move is UB the compiler
// will not catch; it can corrupt the program's own view of which slot holds
// what, and it cannot make the kernel touch freed memory. doc/Notes.md has the
// argument in full.

#ifndef KORU_POOL_HPP
#define KORU_POOL_HPP

#include <koru/ring.hpp>

#include <cstdint>
#include <span>
#include <vector>

namespace koru {

class BufPool;

/// One slot, exclusively owned. Returns itself to the pool on destruction, and
/// must not outlive it.
class BufSlot {
public:
    BufSlot() = default;
    ~BufSlot();

    BufSlot(const BufSlot &)            = delete;
    BufSlot &operator=(const BufSlot &) = delete;
    BufSlot(BufSlot &&o) noexcept;
    BufSlot &operator=(BufSlot &&o) noexcept;

    /// False once moved from, and for a default-constructed slot.
    bool held() const { return pool_ != nullptr; }

    uint32_t index() const { return index_; }
    std::span<uint8_t> bytes() const;
    size_t size() const { return bytes().size(); }

    uint8_t &operator[](size_t i) const { return bytes()[i]; }

    /// Give it back early. The destructor does this.
    void release();

private:
    friend class BufPool;
    BufSlot(BufPool *pool, uint32_t index) : pool_(pool), index_(index) {}

    BufPool *pool_  = nullptr;
    uint32_t index_ = 0;
};

class BufPool {
public:
    BufPool() = default;
    explicit BufPool(Arena arena);

    BufPool(const BufPool &)            = delete;
    BufPool &operator=(const BufPool &) = delete;

    /// A slot, or one that is not `held()` when the pool is empty.
    BufSlot acquire();

    size_t free_count() const { return free_.size(); }
    uint32_t slot_size() const { return arena_.slot_size(); }
    uint32_t slot_count() const { return arena_.slot_count(); }
    const Arena &arena() const { return arena_; }

private:
    friend class BufSlot;
    void give_back(uint32_t index) { free_.push_back(index); }

    Arena arena_;
    std::vector<uint32_t> free_;
};

} // namespace koru

#endif // KORU_POOL_HPP
