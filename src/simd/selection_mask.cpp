#include "selection_mask.h"

#include <algorithm>

namespace simple_olap::simd {

void SelectionMask::ClearUnusedBits() {
    if (row_count_ == 0) {
        words_.fill(0);
        return;
    }

    const uint32_t used_words = (row_count_ + kWordBits - 1) / kWordBits;

    // [used_words, kWordCount) 全部清零
    for (uint32_t i = used_words; i < kWordCount; ++i) {
        words_[i] = 0;
    }

    // 最后一个有效 word 内超出 row_count_ 的位清零
    const uint32_t rem = row_count_ % kWordBits;
    if (rem != 0 && used_words > 0) {
        words_[used_words - 1] &= (uint64_t(1) << rem) - 1;
    }
}

void SelectionMask::SetAll(uint32_t row_count) {
    row_count_ = std::min(row_count, kVectorBatchSize);
    words_.fill(~uint64_t(0));
    ClearUnusedBits();
}

void SelectionMask::SetNone(uint32_t row_count) {
    row_count_ = std::min(row_count, kVectorBatchSize);
    words_.fill(0);
}

void SelectionMask::FromSelectionVector(const std::vector<uint32_t>& selection, uint32_t physical_count) {
    SetNone(physical_count);
    for (uint32_t row : selection) {
        if (row < row_count_) {
            words_[row >> 6] |= (uint64_t(1) << (row & 63));
        }
    }
}

void SelectionMask::And(const SelectionMask& rhs) {
    row_count_ = std::min(row_count_, rhs.row_count_);
    for (uint32_t i = 0; i < kWordCount; ++i) {
        words_[i] &= rhs.words_[i];
    }
    ClearUnusedBits();
}

void SelectionMask::Or(const SelectionMask& rhs) {
    row_count_ = std::max(row_count_, rhs.row_count_);
    for (uint32_t i = 0; i < kWordCount; ++i) {
        words_[i] |= rhs.words_[i];
    }
    ClearUnusedBits();
}

void SelectionMask::AndNot(const SelectionMask& rhs) {
    for (uint32_t i = 0; i < kWordCount; ++i) {
        words_[i] &= ~rhs.words_[i];
    }
    ClearUnusedBits();
}

bool SelectionMask::Empty() const {
    for (uint32_t i = 0; i < kWordCount; ++i) {
        if (words_[i] != 0) {
            return false;
        }
    }
    return true;
}

bool SelectionMask::IsAll() const {
    if (row_count_ == 0) {
        return true; // 空集合：vacuous truth
    }

    const uint32_t used_words = (row_count_ + kWordBits - 1) / kWordBits;

    // 除最后一个有效 word 外，必须全 1
    for (uint32_t i = 0; i + 1 < used_words; ++i) {
        if (words_[i] != ~uint64_t(0)) {
            return false;
        }
    }

    // 最后一个有效 word 只检查 [0, row_count_) 内的位；
    // 超出 row_count_ 的位按不变式恒为 0，不能参与判断。
    const uint32_t rem = row_count_ % kWordBits;
    const uint64_t last_mask = (rem == 0) ? ~uint64_t(0) : ((uint64_t(1) << rem) - 1);
    return (words_[used_words - 1] & last_mask) == last_mask;
}

uint32_t SelectionMask::Count() const {
    uint32_t total = 0;
    const uint32_t count = (row_count_ + kWordBits - 1) / kWordBits;
    for (uint32_t i = 0; i < count; ++i) {
        total += static_cast<uint32_t>(__builtin_popcountll(words_[i]));
    }
    return total;
}

void SelectionMask::ToSelectionVector(std::vector<uint32_t>& output) const {
    output.clear();
    const uint32_t word_count = (row_count_ + kWordBits - 1) / kWordBits;
    for (uint32_t w = 0; w < word_count; ++w) {
        uint64_t bits = words_[w];
        while (bits != 0) {
            const uint32_t bit = static_cast<uint32_t>(__builtin_ctzll(bits));
            const uint32_t row = w * kWordBits + bit;
            if (row < row_count_) {
                output.push_back(row);
            }
            bits &= bits - 1; // 清最低置位
        }
    }
}

bool SelectionMask::Test(uint32_t row) const {
    if (row >= row_count_) {
        return false;
    }
    return (words_[row >> 6] >> (row & 63)) & 1ULL;
}

} // namespace simple_olap::simd
