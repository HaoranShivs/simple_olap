#pragma once

#include <array>
#include <cstdint>
#include <vector>

#include "../common/constants.h"

namespace simple_olap::simd {

// SelectionMask：固定容量的按位选择掩码（bitmap），一个 batch 一份。
//
// kVectorBatchSize == 1024 -> 16 个 uint64_t -> 128 bytes，可常驻寄存/L1。
//
// Invariant（必须始终成立）：
//   位 [row_count_, kVectorBatchSize) 必须为 0。
//   否则 Count() / IsAll() / Or() 会给出错误结果。
//   所有会写入 words_ 的方法（SetAll/SetNone/And/Or/AndNot/内核直接写 data()）
//   都必须保证这一点；内核直接写 data() 时约定只写 [0, count) 且 count <= 行数。
class SelectionMask {
  public:
    static constexpr uint32_t kWordBits = 64;
    static constexpr uint32_t kWordCount = (kVectorBatchSize + kWordBits - 1) / kWordBits;

    SelectionMask() = default;

    // 全部置位 / 全部清零，并设定有效行数（超出部分保持 0）。
    void SetAll(uint32_t row_count);
    void SetNone(uint32_t row_count);

    // 由 selection vector（物理局部索引）构造：只置位 selection 中的行。
    void FromSelectionVector(const std::vector<uint32_t>& selection, uint32_t physical_count);

    // 按位运算（row_count_ 取两者中较小者，保证不使用越界位）。
    void And(const SelectionMask& rhs);
    void Or(const SelectionMask& rhs);
    void AndNot(const SelectionMask& rhs);

    bool Empty() const;
    bool IsAll() const;

    uint32_t Count() const;

    void ToSelectionVector(std::vector<uint32_t>& output) const;

    bool Test(uint32_t row) const;

    uint64_t* data() {
        return words_.data();
    }
    const uint64_t* data() const {
        return words_.data();
    }

    uint32_t row_count() const noexcept {
        return row_count_;
    }

  private:
    void ClearUnusedBits();

    std::array<uint64_t, kWordCount> words_{};
    uint32_t row_count_ = 0;
};

} // namespace simple_olap::simd
