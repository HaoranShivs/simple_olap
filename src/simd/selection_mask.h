#pragma once

#include <array>
#include <cstdint>
#include <vector>

#include "../common/constants.h"

namespace simple_olap::simd {

class SelectionCursor;

// SelectionMask：固定容量的按位选择掩码（bitmap），一个 batch 一份。
//
// kVectorBatchSize == 1024 -> 16 个 uint64_t -> 128 bytes，可常驻寄存/L1。
//
// Invariant（必须始终成立）：
//   位 [row_count_, kVectorBatchSize) 必须为 0。
//   否则 Count() / IsAll() / Or() 会给出错误结果。
//   所有会写入 words_ 的方法（SetAll/SetNone/And/Or/AndNot/内核直接写 data()）
//   都必须保证这一点；内核直接写 data() 时约定只写 [0, count) 且 count <= 行数。
//
// SelectionMask 是执行期唯一权威的行选择表达：Storage / Filter / Projection /
// Aggregate 之间不再有 mask <-> selection vector 的往返转换。
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

    // 仅供测试 / 诊断 / 与旧接口互操作使用；执行期热路径请用 ForEachSetBit / MakeCursor。
    void ToSelectionVector(std::vector<uint32_t>& output) const;

    // 直接遍历置位行，不产生 selection vector / 不分配内存。
    // fn 的签名：void(uint32_t physical_row)。
    template <typename Func> void ForEachSetBit(Func&& fn) const {
        const uint32_t word_count = (row_count_ + kWordBits - 1) / kWordBits;
        for (uint32_t w = 0; w < word_count; ++w) {
            uint64_t bits = words_[w];
            while (bits != 0) {
                const uint32_t bit = static_cast<uint32_t>(__builtin_ctzll(bits));
                bits &= bits - 1; // 清最低置位
                fn(w * kWordBits + bit);
            }
        }
    }

    // 批量友好的行遍历游标：一次提取最多 capacity 个 physical row index。
    SelectionCursor MakeCursor() const;

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

// SelectionCursor：按 word 顺序推进的置位行游标。
//
// 用途（避免任何模块为了遍历有效行而先 ToSelectionVector()）：
//   - Sparse Projection / Typed Gather：一次取一批 physical index
//   - ScalarVectorPredicate：只 Eval 被选中的行
//   - Aggregate / 最终结果打印：直接遍历 active rows
//
// 语义与 ForEachSetBit 完全一致（同一套 ctz + clear-lowest-bit 逻辑），
// 只是把控制流交给调用方，便于和 SIMD 块处理配合。
class SelectionCursor {
  public:
    explicit SelectionCursor(const SelectionMask& mask) : mask_(&mask) {}

    // 取下一个置位行；返回 false 表示遍历结束。
    bool Next(uint32_t& row);

    // 一次提取最多 capacity 个 physical row index，返回实际写入个数。
    // 返回 0 表示遍历结束。
    uint32_t NextBlock(uint32_t* indices, uint32_t capacity);

  private:
    // 保证 remaining_bits_ 非空；没有更多置位时返回 false。
    bool EnsureBits();

    const SelectionMask* mask_ = nullptr;
    uint32_t next_word_ = 0;       // 下一个待扫描的 word（找非零 bits 用）
    uint32_t remaining_word_ = 0;  // remaining_bits_ 所属的 word
    uint64_t remaining_bits_ = 0;  // 当前 word 尚未消费的置位
};

inline SelectionCursor SelectionMask::MakeCursor() const {
    return SelectionCursor(*this);
}

} // namespace simple_olap::simd
