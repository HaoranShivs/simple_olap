#pragma once

#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <vector>

#include "../block_pool/block_pool.h"

namespace simple_olap {

// Arena：
//   - 基于 BlockPool 的 bump allocator，用于查询生命周期对象
//     （HashAggregate 等 PMR 容器）
//   - 同时实现 std::pmr::memory_resource，可直接作为 PMR 容器的 allocator
//
// 线程归属：Arena 永远不跨线程并发 Allocate（无 mutex、无 atomic）。
// 每个 worker 使用自己的 Arena（见 QueryMemoryContext）。
//
// 生命周期硬性规定：
//   所有使用 Arena 的 PMR 容器必须在 Arena Release() / 析构之前销毁。
class Arena final : public std::pmr::memory_resource {
  public:
    explicit Arena(BlockPool& block_pool);

    ~Arena() override;

    Arena(const Arena&) = delete;
    Arena& operator=(const Arena&) = delete;

    Arena(Arena&&) = delete;
    Arena& operator=(Arena&&) = delete;

    void* Allocate(size_t size, size_t alignment = alignof(std::max_align_t));

    template <typename T> T* AllocateArray(size_t count) {
        return static_cast<T*>(Allocate(sizeof(T) * count, alignof(T)));
    }

    // offset 清零，但保留已经拿到的 blocks（下次分配可复用）。
    void Reset() noexcept;

    // 所有 blocks 归还 BlockPool。
    void Release() noexcept;

    size_t used_bytes() const noexcept;
    size_t reserved_bytes() const noexcept;
    size_t block_count() const noexcept;

  private:
    struct ArenaBlock {
        MemoryBlock memory;
        size_t offset = 0;
    };

    void* AllocateSlow(size_t size, size_t alignment);

    static size_t AlignUp(size_t value, size_t alignment) noexcept;

    void* do_allocate(size_t bytes, size_t alignment) override;

    void do_deallocate(void* p, size_t bytes, size_t alignment) override;

    bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override;

  private:
    BlockPool* block_pool_;

    std::vector<ArenaBlock> blocks_;

    size_t current_block_ = 0;
};

} // namespace simple_olap
