#include "block_pool.h"

#include <new>
#include <utility>

namespace simple_olap {

namespace {

constexpr size_t kAlignment = BlockPool::DEFAULT_ALIGNMENT;

inline size_t AlignUp(size_t value, size_t alignment) noexcept {
    return (value + alignment - 1) & ~(alignment - 1);
}

} // namespace

BlockPool::BlockPool(size_t block_size, size_t max_cached_blocks)
    : block_size_(AlignUp(block_size == 0 ? DEFAULT_BLOCK_SIZE : block_size, kAlignment)),
      max_cached_blocks_(max_cached_blocks) {}

BlockPool::~BlockPool() {
    // 析构时释放所有缓存的 block。
    for (MemoryBlock& block : free_blocks_) {
        FreeBlock(block);
    }
    free_blocks_.clear();
}

MemoryBlock BlockPool::Acquire(size_t min_capacity) {
    if (min_capacity <= block_size_) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!free_blocks_.empty()) {
                MemoryBlock block = std::move(free_blocks_.back());
                free_blocks_.pop_back();
                pool_hits_.fetch_add(1, std::memory_order_relaxed);
                return block;
            }
        }

        return AllocateNormalBlock();
    }

    return AllocateLargeBlock(min_capacity);
}

void BlockPool::Release(MemoryBlock&& block) noexcept {
    if (block.data == nullptr) {
        return;
    }

    if (block.pooled) {
        bool cached = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (free_blocks_.size() < max_cached_blocks_) {
                free_blocks_.push_back(std::move(block));
                cached = true;
            }
        }

        if (cached) {
            pool_returns_.fetch_add(1, std::memory_order_relaxed);
            return;
        }

        // freelist 已满：直接归还系统。
        FreeBlock(block);
        return;
    }

    // large allocation：直接归还系统。
    FreeBlock(block);
}

size_t BlockPool::cached_blocks() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return free_blocks_.size();
}

BlockPoolStats BlockPool::stats() const noexcept {
    BlockPoolStats result;
    result.system_allocations = system_allocations_.load(std::memory_order_relaxed);
    result.pool_hits = pool_hits_.load(std::memory_order_relaxed);
    result.pool_returns = pool_returns_.load(std::memory_order_relaxed);

    std::lock_guard<std::mutex> lock(mutex_);
    result.cached_blocks = free_blocks_.size();
    return result;
}

MemoryBlock BlockPool::AllocateNormalBlock() {
    // C++17 aligned new。
    void* memory = ::operator new(block_size_, std::align_val_t{kAlignment});
    system_allocations_.fetch_add(1, std::memory_order_relaxed);

    return MemoryBlock(static_cast<uint8_t*>(memory), block_size_, /*pooled=*/true);
}

MemoryBlock BlockPool::AllocateLargeBlock(size_t capacity) {
    const size_t aligned_capacity = AlignUp(capacity, kAlignment);

    void* memory = ::operator new(aligned_capacity, std::align_val_t{kAlignment});
    system_allocations_.fetch_add(1, std::memory_order_relaxed);

    return MemoryBlock(static_cast<uint8_t*>(memory), aligned_capacity, /*pooled=*/false);
}

void BlockPool::FreeBlock(MemoryBlock& block) noexcept {
    if (block.data == nullptr) {
        return;
    }

    ::operator delete(block.data, std::align_val_t{kAlignment});
    block.data = nullptr;
    block.capacity = 0;
    block.pooled = false;
}

} // namespace simple_olap
