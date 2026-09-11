#pragma once

#include <cstddef>
#include <cstdint>

namespace simple_olap {

// MemoryBlock 是一块裸内存的非拥有描述：
//   - data / capacity ：内存区间
//   - pooled          ：true  = 标准 BlockPool block（Release 时可进 freelist 复用）
//                       false = large allocation（Release 时直接归还系统）
//
// MemoryBlock 自身不释放内存；真正的所有权管理由 BlockPool / Arena 共同完成。
// move 之后源对象置空，但同样不释放——释放责任始终在持有方（BlockPool::Release）。
struct MemoryBlock {
    uint8_t* data = nullptr;
    size_t capacity = 0;

    // true：标准 BlockPool block，可以进入 freelist。
    // false：large allocation，Release 时直接归还系统。
    bool pooled = false;

    MemoryBlock() = default;

    MemoryBlock(uint8_t* data, size_t capacity, bool pooled) : data(data), capacity(capacity), pooled(pooled) {}

    MemoryBlock(const MemoryBlock&) = delete;
    MemoryBlock& operator=(const MemoryBlock&) = delete;

    MemoryBlock(MemoryBlock&& other) noexcept : data(other.data), capacity(other.capacity), pooled(other.pooled) {
        other.data = nullptr;
        other.capacity = 0;
        other.pooled = false;
    }

    MemoryBlock& operator=(MemoryBlock&& other) noexcept {
        if (this != &other) {
            data = other.data;
            capacity = other.capacity;
            pooled = other.pooled;
            other.data = nullptr;
            other.capacity = 0;
            other.pooled = false;
        }
        return *this;
    }

    explicit operator bool() const noexcept {
        return data != nullptr;
    }
};

} // namespace simple_olap
