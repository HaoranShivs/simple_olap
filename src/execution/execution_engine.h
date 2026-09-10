#pragma once

#include <cstdint>
#include <functional>

#include "../planner/physical_plan/physical_plan.h"
#include "execution_context.h"
#include "executor_builder.h"
#include "schema.h"
#include "vector/vector.h"

namespace simple_olap {

enum class ExecutionResultType : uint8_t {
    QUERY,   // SELECT：流式产出 batch
    COMMAND, // INSERT / CREATE_TABLE：一次性命令
};

// 执行结果：
//   QUERY   -> schema + row_count（数据通过 BatchConsumer 流式交给上层）
//   COMMAND -> affected_rows
struct ExecutionResult {
    ExecutionResultType type = ExecutionResultType::QUERY;

    // QUERY：输出列 schema
    ExecSchema schema;

    // QUERY：总行数
    uint64_t row_count = 0;

    // COMMAND：受影响行数
    uint64_t affected_rows = 0;
};

// 每个 batch 产出时回调一次：参数为 (batch, 输出 schema)。
// 回调返回后上层不得继续持有 batch 内数据的裸引用。
using BatchConsumer = std::function<void(const VectorBatch&, const ExecSchema&)>;

// 顶层执行引擎：根据物理计划类型分发。
//   查询计划（SEQ_SCAN / FILTER / PROJECT / HASH_AGGREGATE）
//     -> ExecutorBuilder 构建算子树，Volcano pull 到 EOF
//   命令计划（INSERT / CREATE_TABLE）
//     -> CommandExecutor 一次性执行
class ExecutionEngine {
  public:
    explicit ExecutionEngine(ExecutionContext* ctx);

    // 统一入口：自动区分查询计划与命令计划
    ExecutionResult Execute(const PhysicalPlan& plan, const BatchConsumer& consumer = nullptr);

  private:
    bool IsQueryPlan(const PhysicalPlan& plan) const;

    ExecutionResult ExecuteQuery(const PhysicalPlan& plan, const BatchConsumer& consumer);

    ExecutionResult ExecuteCommand(const PhysicalPlan& plan);

    ExecutionContext* ctx_ = nullptr;
};

class ParallelExecutionEngine {
  public:
    explicit ParallelExecutionEngine(ExecutionContext* ctx);

    // 形成 operator 树后，将 Aggregate
    // 作为分界线，分割为2个树，代表聚合后数据处理，和聚合前数据处理。继续细分Aggregate，将其分为两部分：上述聚合前数据处理+形成部分group表为多线程聚合前数据处理部分，将多个部分group表合并为最终Aggregate为聚合后处理部分。其中，聚合前数据处理部分采用多线程运行，聚合后数据处理采用单线程。
    // 多线程的开启和结束都在Execute里，这也就意味着在Execute将operator 树拆分两部分，再将Aggregate拆分两部分。
    ExecutionResult Execute(const PhysicalPlan& plan, const BatchConsumer& consumer = nullptr);

  private:
    ExecutionResult ExecuteQuery(const PhysicalPlan& plan, const BatchConsumer& consumer);

    // 聚合查询的并行执行：
    //   聚合前阶段（多线程）：每个 worker 构建独立的「聚合前子树」
    //     （Scan -> Filter -> Project -> 部分 Aggregate），从共享 segment
    //     分配器领取互不重叠的 segment，产出本地部分 group 表。
    //   聚合后阶段（单线程）：把所有部分 group 表按聚合语义合并成
    //     最终 group 表，再 finalize 并通过 consumer 流式产出。
    ExecutionResult ExecuteParallelAggregate(const PhysicalPlan& plan, const BatchConsumer& consumer);

    // 计算并行度：min(线程池线程数, segment 数)；至少 1
    size_t CalculateParallelism(size_t segment_count) const;

    ExecutionContext* ctx_ = nullptr;
};

} // namespace simple_olap
