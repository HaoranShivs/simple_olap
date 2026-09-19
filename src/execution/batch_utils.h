#pragma once

#include <cstdint>

#include "vector/vector.h"

namespace simple_olap {

// 统一 Batch 语义（全执行期不变式）：
//
//   selection.row_count() == physical row count
//   ColumnData::count      == physical row count
//   VectorBatch::size      == active row count
//
//   dense（selection.IsAll()）:
//       size == physical count
//   sparse（!selection.IsAll()）:
//       size == selection.Count()
//
// SelectionMask 是唯一权威 selection；不再有 selection vector 副本。
inline uint32_t ActiveRowCount(const VectorBatch& batch) {
    return batch.size;
}

inline uint32_t PhysicalRowCount(const VectorBatch& batch) {
    return batch.PhysicalSize();
}

// 遍历有效行：dense 走最朴素的连续 for（不查 mask），sparse 走 mask 置位遍历。
// 优化粒度保持在「行遍历」这一层，调用方不再关心 selection 表示。
template <typename Func> inline void ForEachActiveRow(const VectorBatch& batch, Func&& fn) {
    if (batch.IsDense()) {
        const uint32_t count = batch.size;
        for (uint32_t row = 0; row < count; ++row) {
            fn(row);
        }
    } else {
        batch.selection().ForEachSetBit(fn);
    }
}

inline void ClearBatch(VectorBatch& batch) {
    batch.Reset();
}

} // namespace simple_olap
