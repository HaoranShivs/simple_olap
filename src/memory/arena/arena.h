#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace simple_olap {

// 最小 Arena 实现：查询期临时内存的批量分配与整体释放。
// 分配只前进指针，Reset() 一次性回收全部内存（不逐个析构），
// 因此只适合存放 trivially destructible 的数据（列缓冲、sel vector 等）。
class Arena {
  public:
    explicit Arena(size_t initial_size = 1 << 20) : buffer_() {
        buffer_.reserve(initial_size);
    }

    // 对齐分配：返回 offset，后续通过 base() + offset 访问
    uint8_t* Allocate(size_t size, size_t alignment = alignof(std::max_align_t)) {
        offset_ = (offset_ + alignment - 1) & ~(alignment - 1);
        if (offset_ + size > buffer_.size()) {
            buffer_.resize(offset_ + size);
        }
        uint8_t* ptr = buffer_.data() + offset_;
        offset_ += size;
        return ptr;
    }

    // 一次性回收全部内存（不调用析构函数）
    void Reset() {
        offset_ = 0;
        buffer_.clear();
    }

    size_t used() const noexcept {
        return offset_;
    }

  private:
    std::vector<uint8_t> buffer_;
    size_t offset_ = 0;
};

} // namespace simple_olap
