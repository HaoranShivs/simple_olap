#pragma once

#include <cstdint>
#include <vector>

#include "../../execution/vector/vector.h"
#include "storage_predicate.h"

namespace simple_olap {

// StorageRowFilter：把已准备好的 pushed predicates 应用到物理 batch 上。
//
// 模型（mask-native，替换旧的“mask -> selection vector”收缩）：
//
//   predicate 1 -> SelectionMask
//   predicate 2 -> SelectionMask
//   ...
//        mask1 & mask2 & ... -> final mask -> batch.selection
//
// metadata 已证明 ALL_MATCH 的 predicate（row_filter_mask[p] == 0）直接跳过。
class StorageRowFilter {
  public:
    // 输入 batch 处于“刚扫描完”的状态：
    //   batch.selection 必须是 identity（SegmentReader 产出时已保证），
    //   batch.PhysicalSize() == 物理行数。
    // 输出：
    //   全部通过 -> batch.SetIdentitySelection(physical_count)
    //   部分通过 -> batch.SetSelection(final_mask)（size = Count）
    static void Apply(const PreparedScanPredicates& prepared, const std::vector<uint8_t>& row_filter_mask,
                      VectorBatch& batch);
};

} // namespace simple_olap
