#include "buffer_pool.h"

#include <new>
#include <utility>

namespace simple_olap {

// ---- BufferHandle ----

BufferHandle::BufferHandle(BufferPool* pool, uint8_t* data, size_t capacity, uint32_t size_class)
    : pool_(pool), data_(data), capacity_(capacity), size_class_(size_class) {}

BufferHandle::BufferHandle(BufferHandle&& other) noexcept
    : pool_(other.pool_), data_(other.data_), capacity_(other.capacity_), size_class_(other.size_class_) {
    other.pool_ = nullptr;
    other.data_ = nullptr;
    other.capacity_ = 0;
    other.size_class_ = UINT32_MAX;
}

BufferHandle& BufferHandle::operator=(BufferHandle&& other) noexcept {
    if (this != &other) {
        Reset();

        pool_ = other.pool_;
        data_ = other.data_;
        capacity_ = other.capacity_;
        size_class_ = other.size_class_;

        other.pool_ = nullptr;
        other.data_ = nullptr;
        other.capacity_ = 0;
        other.size_class_ = UINT32_MAX;
    }
    return *this;
}

BufferHandle::~BufferHandle() {
    Reset();
}

void BufferHandle::Reset() noexcept {
    if (data_ != nullptr) {
        pool_->Release(data_, capacity_, size_class_);
    }

    pool_ = nullptr;
    data_ = nullptr;
    capacity_ = 0;
    size_class_ = UINT32_MAX;
}

// ---- BufferPool ----

namespace {

inline size_t AlignUp(size_t value, size_t alignment) noexcept {
    return (value + alignment - 1) & ~(alignment - 1);
}

} // namespace

BufferPool::BufferPool(size_t max_cached_per_class) : max_cached_per_class_(max_cached_per_class) {}

BufferPool::~BufferPool() {
    for (FreeList& free_list : free_lists_) {
        for (uint8_t* buffer : free_list.buffers) {
            ::operator delete(buffer, std::align_val_t{ALIGNMENT});
        }
        free_list.buffers.clear();
    }
}

BufferHandle BufferPool::Acquire(size_t min_capacity) {
    // 找到最小的能容纳请求的 size class。
    for (size_t i = 0; i < kClassCount; ++i) {
        if (min_capacity <= SIZE_CLASSES[i]) {
            FreeList& free_list = free_lists_[i];

            {
                std::lock_guard<std::mutex> lock(free_list.mutex);
                if (!free_list.buffers.empty()) {
                    uint8_t* buffer = free_list.buffers.back();
                    free_list.buffers.pop_back();
                    pool_hits_.fetch_add(1, std::memory_order_relaxed);
                    return BufferHandle(this, buffer, SIZE_CLASSES[i], static_cast<uint32_t>(i));
                }
            }

            void* memory = ::operator new(SIZE_CLASSES[i], std::align_val_t{ALIGNMENT});
            system_allocations_.fetch_add(1, std::memory_order_relaxed);
            return BufferHandle(this, static_cast<uint8_t*>(memory), SIZE_CLASSES[i], static_cast<uint32_t>(i));
        }
    }

    // 超过最大 size class：direct allocation。
    const size_t capacity = AlignUp(min_capacity, ALIGNMENT);
    void* memory = ::operator new(capacity, std::align_val_t{ALIGNMENT});
    system_allocations_.fetch_add(1, std::memory_order_relaxed);
    return BufferHandle(this, static_cast<uint8_t*>(memory), capacity, UINT32_MAX);
}

void BufferPool::Release(uint8_t* data, size_t capacity, uint32_t size_class) noexcept {
    if (data == nullptr) {
        return;
    }

    if (size_class == UINT32_MAX) {
        // direct allocation：直接归还系统。
        ::operator delete(data, std::align_val_t{ALIGNMENT});
        return;
    }

    FreeList& free_list = free_lists_[size_class];

    {
        std::lock_guard<std::mutex> lock(free_list.mutex);
        if (free_list.buffers.size() < max_cached_per_class_) {
            free_list.buffers.push_back(data);
            pool_returns_.fetch_add(1, std::memory_order_relaxed);
            return;
        }
    }

    // cache 已满：直接归还系统。
    ::operator delete(data, std::align_val_t{ALIGNMENT});
}

BufferPoolStats BufferPool::stats() const noexcept {
    BufferPoolStats result;
    result.system_allocations = system_allocations_.load(std::memory_order_relaxed);
    result.pool_hits = pool_hits_.load(std::memory_order_relaxed);
    result.pool_returns = pool_returns_.load(std::memory_order_relaxed);
    return result;
}

} // namespace simple_olap
