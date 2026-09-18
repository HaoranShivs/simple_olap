#pragma once

#include <cstdint>
#include <memory>
#include <vector>

#include "../../storage/datastructs.h"
#include "../../storage/index/key_encoder.h"
#include "../../storage/scan/storage_predicate.h"
#include "../execution_context.h"
#include "../operator.h"

namespace simple_olap {

class TableStorage;

// 索引点查算子：
//
//   PhysicalIndexScan
//        │
//        ▼
//   TableStorage::Lookup(key_id, encoded_key)   -> RowLocation 列表
//        │  （按 segment 分组）
//        ▼
//   TableStorage::ReadRows(segment_id, offsets, columns)
//        │
//        ▼
//   VectorBatch（residual predicates 逐行精确过滤）
//
// 只读取索引命中的行，不扫描整表；输出列与 SeqScan 一致，
// 因此上层 Filter / Projection 无需感知访问路径差异。
class IndexScanOperator final : public Operator {
  public:
    IndexScanOperator(TableId table_id, KeyId key_id, std::vector<ScalarValue> lookup_values,
                      std::vector<ColumnId> columns, std::vector<Condition> residual_predicates, ExecutionContext* ctx)
        : table_id_(table_id), key_id_(key_id), lookup_values_(std::move(lookup_values)),
          columns_(std::move(columns)), residual_predicates_(std::move(residual_predicates)), ctx_(ctx) {}

    void Init() override;
    bool Next(VectorBatch& batch) override;

  private:
    // 同一 segment 的命中行合并成一次 ReadRows（columnar segment 以段为单位读取）
    struct SegmentRows {
        SegmentId segment_id = 0;
        std::vector<uint32_t> row_offsets;
    };

    TableId table_id_;
    KeyId key_id_;
    std::vector<ScalarValue> lookup_values_;
    std::vector<ColumnId> columns_;
    std::vector<Condition> residual_predicates_;
    ExecutionContext* ctx_ = nullptr;

    TableStorage* table_ = nullptr;

    std::vector<SegmentRows> groups_;
    size_t next_group_ = 0;
    size_t next_offset_ = 0;

    // residual predicates 的一次性准备结果（与 SeqScan 的行过滤共用同一套内核）
    std::shared_ptr<const PreparedScanPredicates> prepared_;
};

} // namespace simple_olap
