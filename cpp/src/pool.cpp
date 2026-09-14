// SPDX-License-Identifier: MIT

#include <koru/pool.hpp>

#include <utility>

namespace koru {

BufPool::BufPool(Arena arena) : arena_(std::move(arena))
{
    free_.reserve(arena_.slot_count());
    for (uint32_t i = arena_.slot_count(); i-- > 0;)
        free_.push_back(i);
}

BufSlot BufPool::acquire()
{
    if (free_.empty())
        return BufSlot();
    uint32_t i = free_.back();
    free_.pop_back();
    return BufSlot(this, i);
}

BufSlot::BufSlot(BufSlot &&o) noexcept : pool_(o.pool_), index_(o.index_)
{
    o.pool_ = nullptr;
}

BufSlot &BufSlot::operator=(BufSlot &&o) noexcept
{
    if (this != &o) {
        release();
        pool_   = o.pool_;
        index_  = o.index_;
        o.pool_ = nullptr;
    }
    return *this;
}

BufSlot::~BufSlot()
{
    release();
}

void BufSlot::release()
{
    if (pool_) {
        pool_->give_back(index_);
        pool_ = nullptr;
    }
}

std::span<uint8_t> BufSlot::bytes() const
{
    if (!pool_)
        return {};
    return pool_->arena().slot(index_);
}

} // namespace koru
