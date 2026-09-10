// src/execution/execution_engine.cpp

#include "execution_engine.h"

#include <stdexcept>

#include "../planner/physical_plan/physical_plan.h"
#include "batch_utils.h"
#include "command_executor.h"
#include "executor_builder.h"
#include "parallel/parallel_executor.h"

namespace simple_olap {

ExecutionEngine::ExecutionEngine(ExecutionContext* ctx) : ctx_(ctx) {
    if (ctx_ == nullptr) {
        throw std::runtime_error("ExecutionEngine requires "
                                 "ExecutionContext");
    }

    // compute pool 由执行层持有（ctx_->thread_pool）；
    // scan pool 由 storage 侧 ParallelScanSession 自持。
    // 线程池不可用时不创建并行执行器，全部走串行路径。
    if (ctx_->thread_pool != nullptr) {
        parallel_executor_ = std::make_unique<ParallelExecutor>(ctx_, ctx_->thread_pool, ctx_->parallel_config);
    }
}

ExecutionEngine::~ExecutionEngine() = default;

bool ExecutionEngine::IsQueryPlan(const PhysicalPlan& plan) const {
    switch (plan.GetType()) {
    case PhysicalPlan::Type::SEQ_SCAN:
    case PhysicalPlan::Type::FILTER:
    case PhysicalPlan::Type::PROJECT:
    case PhysicalPlan::Type::HASH_AGGREGATE:
        return true;

    case PhysicalPlan::Type::INSERT:
    case PhysicalPlan::Type::CREATE_TABLE:
        return false;
    }

    throw std::runtime_error("unknown physical plan type");
}

ExecutionResult ExecutionEngine::Execute(const PhysicalPlan& plan, const BatchConsumer& consumer) {
    if (IsQueryPlan(plan)) {
        return ExecuteQuery(plan, consumer);
    }

    return ExecuteCommand(plan);
}

ExecutionResult ExecutionEngine::ExecuteQuery(const PhysicalPlan& plan, const BatchConsumer& consumer) {
    const ExecutionMode mode = ctx_->execution_mode;

    // 强制单线程：直接走 Volcano pull，完全不触碰线程池。
    if (mode == ExecutionMode::SINGLE_THREAD) {
        return ExecuteSerialQuery(plan, consumer);
    }

    // 可并行的计划形状（聚合或普通查询管道），且线程池可用。
    if (parallel_executor_ != nullptr && CanParallelizeAggregate(plan)) {
        // 聚合形状 -> 并行聚合（局部聚合 + merge）
        if (plan.GetType() == PhysicalPlan::Type::HASH_AGGREGATE) {
            return parallel_executor_->ExecuteAggregate(static_cast<const PhysicalHashAggregate&>(plan), consumer);
        }

        // 普通查询管道 -> 并行扫描 + 并行计算，结果批次流式回传。
        // 仅 MULTI_THREAD 启用；AUTO 下普通 SELECT 保持串行，避免改变既有行为。
        if (mode == ExecutionMode::MULTI_THREAD) {
            return parallel_executor_->ExecuteQuery(plan, consumer);
        }
    }

    // 其余情况（AUTO 下的普通 SELECT、不可并行形状、无可用线程池）走串行。
    return ExecuteSerialQuery(plan, consumer);
}

ExecutionResult ExecutionEngine::ExecuteSerialQuery(const PhysicalPlan& plan, const BatchConsumer& consumer) {
    // ---------- 1. PhysicalPlan -> Operator Tree ----------

    ExecutorBuilder builder(ctx_);

    BuiltExecutor executor = builder.Build(plan);

    if (!executor.root) {
        throw std::runtime_error("ExecutorBuilder returned "
                                 "empty operator tree");
    }

    ExecutionResult result;

    result.type = ExecutionResultType::QUERY;

    result.schema = executor.output_schema;

    // ---------- 2. 初始化整个 Operator Tree ----------

    executor.root->Init();

    // ---------- 3. Pull execution ----------

    VectorBatch batch;

    while (true) {
        // 每次 Next 都要求产生一个新的逻辑 batch。
        batch.Reset();

        if (!executor.root->Next(batch)) {
            break;
        }

        const uint32_t active_rows = ActiveRowCount(batch);

        // 约定 Next() == true 时 batch 应尽量非空；此处保守跳过空批次
        if (active_rows == 0) {
            continue;
        }

        result.row_count += active_rows;

        // 把 batch 交给上层

        if (consumer) {
            consumer(batch, result.schema);
        }

        // 回调返回后，上层不得继续持有 batch 数据的裸引用
    }

    return result;
}

ExecutionResult ExecutionEngine::ExecuteCommand(const PhysicalPlan& plan) {
    CommandExecutor executor(ctx_);

    CommandResult command_result = executor.Execute(plan);

    ExecutionResult result;

    result.type = ExecutionResultType::COMMAND;

    result.affected_rows = command_result.affected_rows;

    return result;
}

} // namespace simple_olap
