#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <vector>

#include "../memory_block.h"

namespace simple_olap {

// BlockPool 统计信息，只用于 benchmark / debug / test，不参与执行逻辑。
struct BlockPoolStats {
    uint64_t system_allocations = 0; // 直接向系统申请的次数
    uint64_t pool_hits = 0;          // 从 freelist 命中的次数
    uint64_t pool_returns = 0;       // 归还进 freelist 的次数
    size_t cached_blocks = 0;        // 当前缓存的 block 数
};

// BlockPool：
//   - 提供大块、地址稳定的内存（默认 1 MB、64 字节对齐）
//   - 标准块（capacity <= block_size）走 freelist 复用
//   - 超过 block_size 的请求走 large allocation，Release 时直接归还系统
//
// 线程安全：所有接口可被多个 Arena 并发调用（内部 mutex + atomic）。
class BlockPool {
  public:
    static constexpr size_t DEFAULT_BLOCK_SIZE = 1 << 20; // 1 MB
    static constexpr size_t DEFAULT_ALIGNMENT = 64;

    explicit BlockPool(size_t block_size = DEFAULT_BLOCK_SIZE, size_t max_cached_blocks = 64);

    ~BlockPool();

    BlockPool(const BlockPool&) = delete;
    BlockPool& operator=(const BlockPool&) = delete;

    // 获取一块容量 >= min_capacity 的内存。
    // min_capacity <= block_size 时返回标准块（capacity == block_size），
    // 否则返回 large block（capacity >= min_capacity）。
    MemoryBlock Acquire(size_t min_capacity);

    // 归还一块内存。pooled 块进 freelist（未满时），否则 / large 块直接释放。
    void Release(MemoryBlock&& block) noexcept;

    size_t block_size() const noexcept {
        return block_size_;
    }

    size_t cached_blocks() const noexcept;

    BlockPoolStats stats() const noexcept;

    uint64_t system_allocations() const noexcept {
        return system_allocations_.load(std::memory_order_relaxed);
    }

  private:
    MemoryBlock AllocateNormalBlock();

    MemoryBlock AllocateLargeBlock(size_t capacity);

    static void FreeBlock(MemoryBlock& block) noexcept;

  private:
    size_t block_size_;
    size_t max_cached_blocks_;

    mutable std::mutex mutex_;
    std::vector<MemoryBlock> free_blocks_;

    std::atomic<uint64_t> system_allocations_{0};
    std::atomic<uint64_t> pool_hits_{0};
    std::atomic<uint64_t> pool_returns_{0};
};

} // namespace simple_olap
