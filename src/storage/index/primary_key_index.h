#pragma once

#include <optional>
#include <unordered_map>
#include <vector>

#include "key_encoder.h"

namespace simple_olap {

// ==========================================
// 主键索引
// ==========================================
// 职责：主键唯一性校验 + 等值点查。
//
// 唯一性对"已插入"的所有键生效（含仍在内存、visible=false 的条目）；
// 点查只返回 visible 条目，保证 SELECT 看到的数据与 SeqScan 一致。
class PrimaryKeyIndex {
  public:
    // 校验一批键是否与已有键或批内自身重复；不修改任何状态。
    // 多行 INSERT 必须整批通过后才允许写入，避免留下半条 SQL 的数据。
    bool ValidateBatch(const std::vector<EncodedKey>& keys) const;

    // 插入一个键；visible=false 表示数据尚未落盘（查询不可见）
    void Insert(EncodedKey key, RowLocation location, bool visible);

    // 等值查找可见条目；不存在或尚未落盘返回 nullopt
    std::optional<RowLocation> Lookup(const EncodedKey& key) const;

    // segment 落盘后调用：把该 segment 的条目标记为对查询可见
    void MarkSegmentVisible(SegmentId segment_id);

  private:
    std::unordered_map<EncodedKey, IndexEntry, EncodedKeyHash> entries_;
};

} // namespace simple_olap
