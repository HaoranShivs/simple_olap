#pragma once

#include <cstdint>
#include <functional>
#include <memory>

#include "../parallel/parallel_config.h"
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

class ParallelExecutor;

// 顶层执行引擎：根据物理计划类型与 ctx_->execution_mode 分发。
//   查询计划（SEQ_SCAN / FILTER / PROJECT / HASH_AGGREGATE）
//     -> 按执行模式选择单线程 / 多线程：
//          SINGLE_THREAD : 始终走 ExecutorBuilder + Volcano pull 串行路径
//          MULTI_THREAD  : 可并行计划走 ParallelExecutor，否则回退串行
//          AUTO          : 可并行聚合走 ParallelExecutor，其余串行（默认）
//   命令计划（INSERT / CREATE_TABLE）
//     -> CommandExecutor 一次性执行（与执行模式无关）
class ExecutionEngine {
  public:
    explicit ExecutionEngine(ExecutionContext* ctx);
    ~ExecutionEngine();

    ExecutionEngine(const ExecutionEngine&) = delete;
    ExecutionEngine& operator=(const ExecutionEngine&) = delete;

    // 统一入口：自动区分查询计划与命令计划
    ExecutionResult Execute(const PhysicalPlan& plan, const BatchConsumer& consumer = nullptr);

  private:
    bool IsQueryPlan(const PhysicalPlan& plan) const;

    // 查询入口分流（受 ctx_->execution_mode 控制）：
    //   SINGLE_THREAD -> 始终串行
    //   MULTI_THREAD  -> 可并行计划（聚合 / 查询管道）走 ParallelExecutor
    //   AUTO          -> 只有可并行聚合走 ParallelExecutor，其余串行
    ExecutionResult ExecuteQuery(const PhysicalPlan& plan, const BatchConsumer& consumer);

    // 串行执行路径（fallback）：ExecutorBuilder 构建算子树后 Volcano pull
    ExecutionResult ExecuteSerialQuery(const PhysicalPlan& plan, const BatchConsumer& consumer);

    ExecutionResult ExecuteCommand(const PhysicalPlan& plan);

    ExecutionContext* ctx_ = nullptr;

    // 并行聚合执行器（compute pool = ctx_->thread_pool；
    // scan pool 由 ParallelScanSession 在 storage 侧自持）。
    // 线程池不可用时为空，全部查询走串行路径。
    std::unique_ptr<ParallelExecutor> parallel_executor_;
};

} // namespace simple_olap
