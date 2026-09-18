#pragma once

#include <unordered_map>
#include <vector>

#include "key_encoder.h"

namespace simple_olap {

// ==========================================
// 二级键索引（非唯一）
// ==========================================
// 职责：普通 KEY 的等值查找，一个键对应多个物理位置。
// 不参与任何唯一性约束；可见性语义与 PrimaryKeyIndex 相同：
// 条目先以 visible=false 插入，segment 落盘后才对查询可见。
class SecondaryKeyIndex {
  public:
    // 插入一个键 -> 位置的映射条目；visible=false 表示数据尚未落盘
    void Insert(EncodedKey key, RowLocation location, bool visible);

    // 等值查找可见位置；无匹配返回空 vector
    std::vector<RowLocation> LookupEqual(const EncodedKey& key) const;

    // segment 落盘后调用：把该 segment 的条目标记为对查询可见
    void MarkSegmentVisible(SegmentId segment_id);

  private:
    std::unordered_map<EncodedKey, std::vector<IndexEntry>, EncodedKeyHash> entries_;
};

} // namespace simple_olap
