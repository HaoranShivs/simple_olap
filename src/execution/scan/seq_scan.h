#pragma once

#include <memory>

#include "../../storage/datastructs.h"
#include "../../storage/scan/parallel_scan_state.h"
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
//   TableStorage::Scan(...)          （串行模式：cursor 自持进度）
//   或 TableStorage::ScanParallel()  （并行模式：morsel-driven 领取 segment）
//        │
//        ▼
//   VectorBatch
//
// 串行模式：直接持有 TableStorage + ScanCursor，Next() 调用 table_->Scan()。
// 并行模式：持有查询级 ParallelScanGlobalState 指针（多个 worker 共享）
// 与 worker 本地的 ParallelScanLocalState；Next() 调用 table_->ScanParallel()，
// 由 storage 原子分配 segment。本算子不感知线程 / 队列 / segment 分配细节，
// Filter / Projection 完全不用修改。
class SeqScanOperator final : public Operator {
  public:
    // 串行构造：直接访问 Storage
    SeqScanOperator(TableId table_id, ScanOptions options, ExecutionContext* ctx)
        : table_id_(table_id), options_(std::move(options)), ctx_(ctx) {}

    // 并行构造：消费查询级扫描状态（worker 本地状态由算子自持）
    SeqScanOperator(TableId table_id, ScanOptions options, ParallelScanGlobalState* parallel_state,
                    ExecutionContext* ctx)
        : table_id_(table_id), options_(std::move(options)), parallel_state_(parallel_state), ctx_(ctx) {}

    void Init() override;
    bool Next(VectorBatch& batch) override;

  private:
    TableId table_id_;
    ScanOptions options_;

    // 并行模式：由 ParallelExecutor 创建，生命周期覆盖整条 pipeline。
    ParallelScanGlobalState* parallel_state_ = nullptr;

    // 并行模式：本 worker 独有的 segment 推进状态。
    ParallelScanLocalState local_state_;

    ExecutionContext* ctx_ = nullptr;
    TableStorage* table_ = nullptr;
    ScanCursor cursor_{};
};

} // namespace simple_olap
