#pragma once

#include <cstdint>

#include "vector/vector.h"

namespace simple_olap {

// 统一 Batch 语义（全执行期不变式）：
//
//   ColumnData::count   = physical row count
//   VectorBatch::size    = active row count
//
//   sel_vector.empty():
//       physical row == logical row
//       size == physical count
//
//   !sel_vector.empty():
//       size == sel_vector.size()
//       ColumnData::count >= size
//
// 因此 ActiveRowCount 恒等于 batch.size；不再存在“sel_vector 更权威”
// 之类的旧 storage 兼容逻辑。
inline uint32_t ActiveRowCount(const VectorBatch& batch) {
    return batch.size;
}

inline uint32_t ActiveRowIndex(const VectorBatch& batch, uint32_t logical_row) {
    return batch.sel_vector.empty() ? logical_row : batch.sel_vector[logical_row];
}

inline uint32_t PhysicalRowCount(const VectorBatch& batch) {
    if (!batch.columns.empty()) {
        return batch.columns.front().count;
    }
    return batch.size;
}

inline bool IsIdentitySelection(const std::vector<uint32_t>& sel, uint32_t physical_count) {
    if (sel.size() != physical_count) {
        return false;
    }
    for (uint32_t i = 0; i < physical_count; ++i) {
        if (sel[i] != i) {
            return false;
        }
    }
    return true;
}

inline void ClearBatch(VectorBatch& batch) {
    for (auto& col : batch.columns) {
        col.Reset();
    }
    batch.columns.clear();
    batch.sel_vector.clear();
    batch.size = 0;
}

} // namespace simple_olap
