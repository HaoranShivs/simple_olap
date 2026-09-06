#pragma once

#include <cstdint>

#include "vector/vector.h"

namespace simple_olap {

// Compatibility rule:
// - New execution semantics: batch.size is active row count.
// - Current storage master still leaves batch.size as scanned row count when
//   sel_vector is non-empty. In that case sel_vector is authoritative.
inline uint32_t ActiveRowCount(const VectorBatch &batch) {
    return batch.sel_vector.empty()
               ? batch.size
               : static_cast<uint32_t>(batch.sel_vector.size());
}

inline uint32_t ActiveRowIndex(const VectorBatch &batch, uint32_t logical_row) {
    return batch.sel_vector.empty() ? logical_row : batch.sel_vector[logical_row];
}

inline uint32_t PhysicalRowCount(const VectorBatch &batch) {
    if (!batch.columns.empty()) {
        return batch.columns.front().count;
    }
    return batch.size;
}

inline bool IsIdentitySelection(const std::vector<uint32_t> &sel, uint32_t physical_count) {
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

inline void ClearBatch(VectorBatch &batch) {
    for (auto &col : batch.columns) {
        col.Reset();
    }
    batch.columns.clear();
    batch.sel_vector.clear();
    batch.size = 0;
}

} // namespace simple_olap
