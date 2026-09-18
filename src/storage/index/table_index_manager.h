#pragma once

#include <memory>
#include <vector>

#include "../../type.h"
#include "../datachunk.h"
#include "../datastructs.h"
#include "primary_key_index.h"
#include "secondary_key_index.h"

namespace simple_olap {

class VectorBatch;

// ==========================================
// 单表索引管理器
// ==========================================
// 持有该表的全部键索引（主键 + 二级键），只负责键编码与索引维护，
// 不感知 segment 文件布局；由 TableStorage 在 Append / Flush / Open 时驱动。
class TableIndexManager {
  public:
    // schema 的权威来源在 Catalog，生命周期必须覆盖本对象。
    explicit TableIndexManager(const TableSchema& schema);

    // 所有键列的并集（去重升序）：重建索引时按这份清单读取 segment
    const std::vector<ColumnId>& key_columns() const noexcept {
        return key_columns_;
    }

    // ---------- 唯一性 ----------

    // 主键唯一性校验（批内重复 + 与已写入数据重复），纯检查、无副作用。
    // 多行 INSERT 必须整批通过后才允许真正写入。
    bool ValidatePrimaryKey(const DataChunk& chunk) const;

    // ---------- 索引维护 ----------

    // 新插入一批行：locations 与 chunk 行一一对应，条目以不可见状态加入
    void OnAppend(const DataChunk& chunk, const std::vector<RowLocation>& locations);

    // 从已落盘 segment 重建索引（TableStorage::Open 时调用）：
    // batch_columns 与 batch.columns 一一对应，locations 与 batch 行一一对应，
    // 条目直接以可见状态加入。
    void RebuildFromBatch(const std::vector<ColumnId>& batch_columns, const VectorBatch& batch,
                          const std::vector<RowLocation>& locations);

    // segment 落盘后调用：这些 segment 的条目对查询可见
    void MarkSegmentsVisible(const std::vector<SegmentId>& segment_ids);

    // ---------- 查找 ----------

    // 按 KeyId 等值查找可见行位置：主键返回 0/1 个，二级键可能多个
    std::vector<RowLocation> Lookup(KeyId key_id, const EncodedKey& key) const;

  private:
    const TableSchema* schema_ = nullptr;

    std::unique_ptr<PrimaryKeyIndex> primary_index_;
    std::vector<std::unique_ptr<SecondaryKeyIndex>> secondary_indexes_;

    std::vector<ColumnId> key_columns_;
};

} // namespace simple_olap
