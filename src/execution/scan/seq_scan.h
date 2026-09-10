#pragma once

#include <memory>

#include "../../storage/datastructs.h"
#include "../../storage/scan/batch_stream.h"
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
//   TableStorage::Scan(ScanOptions)          （串行模式）
//   或 BatchStream::Next()                   （并行模式）
//        │
//        ▼
//   VectorBatch
//
// 串行模式：直接持有 TableStorage + ScanCursor，Next() 调用 table_->Scan()。
// 并行模式：持有共享的 BatchStream（ParallelScanSession），Next() 从
// BoundedBlockingQueue Pop 数据。本算子不感知 SegmentId / scan worker /
// thread id / next_segment，Filter / Projection 完全不用修改。
class SeqScanOperator final : public Operator {
  public:
    // 串行构造：直接访问 Storage
    SeqScanOperator(TableId table_id, ScanOptions options, ExecutionContext* ctx)
        : table_id_(table_id), options_(std::move(options)), ctx_(ctx) {}

    // 并行构造：消费指定的 BatchStream
    SeqScanOperator(TableId table_id, ScanOptions options, std::shared_ptr<BatchStream> stream, ExecutionContext* ctx)
        : table_id_(table_id), options_(std::move(options)), stream_(std::move(stream)), ctx_(ctx) {}

    void Init() override;
    bool Next(VectorBatch& batch) override;

  private:
    TableId table_id_;
    ScanOptions options_;
    std::shared_ptr<BatchStream> stream_;
    ExecutionContext* ctx_ = nullptr;
    TableStorage* table_ = nullptr;
    ScanCursor cursor_{};
};

} // namespace simple_olap
