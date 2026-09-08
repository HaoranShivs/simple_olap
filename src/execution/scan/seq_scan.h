#pragma once

#include "../../storage/datastructs.h"
#include "../execution_context.h"
#include "../operator.h"

namespace simple_olap {

class TableStorage;

// 物理顺序扫描算子：
//
//   PhysicalTableScan
//        │
//        ▼
//   StorageManager::GetTable(table_id)
//        │
//        ▼
//   TableStorage::Scan(ScanOptions)
//        │
//        ▼
//   SegmentReader（zone map pruning）
//        │
//        ▼
//   VectorBatch
class SeqScanOperator final : public Operator {
  public:
    SeqScanOperator(TableId table_id, ScanOptions options, ExecutionContext* ctx)
        : table_id_(table_id), options_(std::move(options)), ctx_(ctx) {}

    void Init() override;
    bool Next(VectorBatch& batch) override;

  private:
    TableId table_id_;
    ScanOptions options_;
    ExecutionContext* ctx_ = nullptr;
    TableStorage* table_ = nullptr;
    ScanCursor cursor_{};
};

} // namespace simple_olap
