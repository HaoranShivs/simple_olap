#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <vector>

namespace simple_olap {

class BufferPool;

// BufferHandle：BufferPool buffer 的 RAII 拥有者。
//   - 析构 / Reset 时把 buffer 归还 BufferPool
//   - handle 本身不共享，但可以安全跨线程 move
//     （worker -> queue -> coordinator -> destroy -> BufferPool）
class BufferHandle {
  public:
    BufferHandle() = default;

    ~BufferHandle();

    BufferHandle(const BufferHandle&) = delete;
    BufferHandle& operator=(const BufferHandle&) = delete;

    BufferHandle(BufferHandle&& other) noexcept;
    BufferHandle& operator=(BufferHandle&& other) noexcept;

    uint8_t* data() noexcept {
        return data_;
    }

    const uint8_t* data() const noexcept {
        return data_;
    }

    size_t capacity() const noexcept {
        return capacity_;
    }

    explicit operator bool() const noexcept {
        return data_ != nullptr;
    }

    // 归还 buffer 给 BufferPool（direct allocation 直接释放）。
    void Reset() noexcept;

  private:
    friend class BufferPool;

    BufferHandle(BufferPool* pool, uint8_t* data, size_t capacity, uint32_t size_class);

  private:
    BufferPool* pool_ = nullptr;
    uint8_t* data_ = nullptr;

    size_t capacity_ = 0;

    // direct allocation 使用特殊值 UINT32_MAX。
    uint32_t size_class_ = UINT32_MAX;
};

// BufferPool 统计信息，只用于 benchmark / debug / test。
struct BufferPoolStats {
    uint64_t system_allocations = 0;
    uint64_t pool_hits = 0;
    uint64_t pool_returns = 0;
};

// BufferPool：提供可 acquire/release 的定长 buffer。
//
// size class 固定（BATCH_SIZE=1024，最大定长类型 VARCHAR 64-byte slot，
// 一个 ColumnData 最大数据块 = 1024 * 64 = 64 KB）：
//   4 KB / 8 KB / 16 KB / 32 KB / 64 KB
// 超过 64 KB：direct allocation（size_class = UINT32_MAX）。
//
// 线程安全：每个 size class 独立 mutex。
class BufferPool {
  public:
    static constexpr size_t ALIGNMENT = 64;

    explicit BufferPool(size_t max_cached_per_class = 64);

    ~BufferPool();

    BufferPool(const BufferPool&) = delete;
    BufferPool& operator=(const BufferPool&) = delete;

    // 获取容量 >= min_capacity 的 buffer。
    BufferHandle Acquire(size_t min_capacity);

    BufferPoolStats stats() const noexcept;

  private:
    friend class BufferHandle;

    void Release(uint8_t* data, size_t capacity, uint32_t size_class) noexcept;

  private:
    struct FreeList {
        std::mutex mutex;
        std::vector<uint8_t*> buffers;
    };

    static constexpr size_t kClassCount = 5;
    static constexpr std::array<size_t, kClassCount> SIZE_CLASSES{4 << 10, 8 << 10, 16 << 10, 32 << 10, 64 << 10};

    std::array<FreeList, kClassCount> free_lists_;

    size_t max_cached_per_class_;

    std::atomic<uint64_t> system_allocations_{0};
    std::atomic<uint64_t> pool_hits_{0};
    std::atomic<uint64_t> pool_returns_{0};
};

} // namespace simple_olap
