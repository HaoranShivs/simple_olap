#pragma once

#include <cstdint>
#include <vector>

#include "../../execution/vector/vector.h"
#include "storage_predicate.h"

namespace simple_olap {

// StorageRowFilter：把已准备好的 pushed predicates 应用到物理 batch 上。
//
// 模型（替换旧的“逐行 double 比较 + 逐个收缩 selection vector”）：
//
//   predicate 1 -> SelectionMask
//   predicate 2 -> SelectionMask
//   ...
//        mask1 & mask2 & ... -> final mask -> sel_vector
//
// metadata 已证明 ALL_MATCH 的 predicate（row_filter_mask[p] == 0）直接跳过。
class StorageRowFilter {
  public:
    // 输入 batch 处于“刚扫描完、无 selection”的状态：
    //   batch.size == 物理行数，batch.sel_vector 为空。
    // 输出：
    //   全部通过 -> sel_vector 清空、size = 物理行数（identity）
    //   部分通过 -> sel_vector = 选中行的 physical 局部索引、size = sel_vector.size()
    static void Apply(const PreparedScanPredicates& prepared, const std::vector<uint8_t>& row_filter_mask,
                      VectorBatch& batch);
};

} // namespace simple_olap
