#include "arena.h"

#include <cassert>
#include <cstring>

namespace simple_olap {

Arena::Arena(BlockPool& block_pool) : block_pool_(&block_pool) {}

Arena::~Arena() {
    Release();
}

size_t Arena::AlignUp(size_t value, size_t alignment) noexcept {
    return (value + alignment - 1) & ~(alignment - 1);
}

void* Arena::Allocate(size_t size, size_t alignment) {
    // 只允许 2 的幂对齐。
    assert(alignment != 0 && (alignment & (alignment - 1)) == 0);

    if (blocks_.empty()) {
        return AllocateSlow(size, alignment);
    }

    ArenaBlock& block = blocks_[current_block_];

    const size_t aligned = AlignUp(block.offset, alignment);

    if (aligned + size <= block.memory.capacity) {
        void* result = block.memory.data + aligned;
        block.offset = aligned + size;
        return result;
    }

    return AllocateSlow(size, alignment);
}

void* Arena::AllocateSlow(size_t size, size_t alignment) {
    // 1. 尝试复用 current 之后的空闲 block（Reset 之后常见）。
    for (size_t i = current_block_ + 1; i < blocks_.size(); ++i) {
        ArenaBlock& candidate = blocks_[i];
        const size_t aligned = AlignUp(candidate.offset, alignment);
        if (aligned + size <= candidate.memory.capacity) {
            current_block_ = i;
            void* result = candidate.memory.data + aligned;
            candidate.offset = aligned + size;
            return result;
        }
    }

    // 2. 向 BlockPool 申请新 block。
    const size_t request = size + alignment;
    MemoryBlock memory = block_pool_->Acquire(request);

    ArenaBlock block;
    block.memory = std::move(memory);
    block.offset = 0;

    blocks_.push_back(std::move(block));
    current_block_ = blocks_.size() - 1;

    ArenaBlock& fresh = blocks_[current_block_];

    const size_t aligned = AlignUp(fresh.offset, alignment);
    void* result = fresh.memory.data + aligned;
    fresh.offset = aligned + size;
    return result;
}

void Arena::Reset() noexcept {
    for (ArenaBlock& block : blocks_) {
        std::memset(block.memory.data, 0, block.offset);
        block.offset = 0;
    }
    current_block_ = 0;
}

void Arena::Release() noexcept {
    for (ArenaBlock& block : blocks_) {
        block_pool_->Release(std::move(block.memory));
    }
    blocks_.clear();
    current_block_ = 0;
}

size_t Arena::used_bytes() const noexcept {
    size_t total = 0;
    for (const ArenaBlock& block : blocks_) {
        total += block.offset;
    }
    return total;
}

size_t Arena::reserved_bytes() const noexcept {
    size_t total = 0;
    for (const ArenaBlock& block : blocks_) {
        total += block.memory.capacity;
    }
    return total;
}

size_t Arena::block_count() const noexcept {
    return blocks_.size();
}

// ---- std::pmr::memory_resource ----

void* Arena::do_allocate(size_t bytes, size_t alignment) {
    return Allocate(bytes, alignment);
}

void Arena::do_deallocate(void* /*p*/, size_t /*bytes*/, size_t /*alignment*/) {
    // no-op：Arena 不支持单对象释放，整体由 Reset / Release 管理。
}

bool Arena::do_is_equal(const std::pmr::memory_resource& other) const noexcept {
    return this == &other;
}

} // namespace simple_olap
