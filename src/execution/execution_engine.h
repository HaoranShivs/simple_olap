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

} // namespace simple_olap
