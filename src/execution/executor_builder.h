#pragma once

#include <memory>
#include <vector>

#include "../planner/physical_plan/physical_plan.h"
#include "../storage/scan/parallel_scan_state.h"
#include "aggregate/hash_aggregate_state.h"
#include "execution_context.h"
#include "expression/exec_expression.h"
#include "operator.h"
#include "schema.h"

namespace simple_olap {

struct BuiltExecutor {
    std::unique_ptr<Operator> root;
    ExecSchema output_schema;
};

// 并行构建上下文：只有 BuildSeqScan() 读取 parallel_scan，
// Filter / Projection 不保存它。
// parallel_scan == nullptr 表示串行 SeqScan。
struct ExecutorBuildOptions {
    ParallelScanGlobalState* parallel_scan = nullptr;
};

// PhysicalSeqScan -> ScanOptions / output schema 的可复用转换结果。
// ParallelExecutor 和普通 ExecutorBuilder 都使用同一份转换逻辑。
struct BuiltScanSpec {
    TableId table_id;
    ScanOptions options;
    ExecSchema output_schema;
};

// PhysicalHashAggregate 的可复用编译结果（不含 child 算子）。
struct BuiltAggregateSpec {
    std::vector<ExecExprPtr> group_exprs;
    std::vector<AggCallSpec> agg_calls;
    std::vector<AggregateOutputSpec> outputs;
    ExecSchema output_schema;
};

class ExecutorBuilder {
  public:
    explicit ExecutorBuilder(ExecutionContext* ctx) : ctx_(ctx) {}

    // Query plans only: SEQ_SCAN / FILTER / PROJECT / HASH_AGGREGATE.
    // INSERT / CREATE_TABLE are command plans and should be handled by a small
    // command executor rather than fake streaming operators.
    BuiltExecutor Build(const PhysicalPlan& plan) const;

    // 并行构建模式：SeqScan 使用查询级 ParallelScanGlobalState
    BuiltExecutor Build(const PhysicalPlan& plan, const ExecutorBuildOptions& options) const;

    // PhysicalSeqScan -> (table_id, ScanOptions, output_schema)
    BuiltScanSpec BuildScanSpec(const PhysicalSeqScan& plan) const;

    // PhysicalHashAggregate -> (group_exprs, agg_calls, outputs, output_schema)
    // child_schema 是聚合 child（SeqScan/Filter/Project 链）的输出 schema。
    BuiltAggregateSpec BuildAggregateSpec(const PhysicalHashAggregate& plan, const ExecSchema& child_schema) const;

  private:
    BuiltExecutor BuildNode(const PhysicalPlan& plan, const ExecutorBuildOptions& options) const;
    BuiltExecutor BuildSeqScan(const PhysicalSeqScan& plan, const ExecutorBuildOptions& options) const;
    BuiltExecutor BuildIndexScan(const PhysicalIndexScan& plan, const ExecutorBuildOptions& options) const;
    BuiltExecutor BuildFilter(const PhysicalFilter& plan, const ExecutorBuildOptions& options) const;
    BuiltExecutor BuildProject(const PhysicalProject& plan, const ExecutorBuildOptions& options) const;
    BuiltExecutor BuildHashAggregate(const PhysicalHashAggregate& plan, const ExecutorBuildOptions& options) const;

    ExecutionContext* ctx_ = nullptr;
};

} // namespace simple_olap
