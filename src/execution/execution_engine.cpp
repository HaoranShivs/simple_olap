// src/execution/execution_engine.cpp

#include "execution_engine.h"

#include <stdexcept>

#include "../planner/physical_plan/physical_plan.h"
#include "batch_utils.h"
#include "command_executor.h"
#include "executor_builder.h"

namespace simple_olap {

ExecutionEngine::ExecutionEngine(ExecutionContext* ctx) : ctx_(ctx) {
    if (ctx_ == nullptr) {
        throw std::runtime_error("ExecutionEngine requires "
                                 "ExecutionContext");
    }
}

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
    // ==========================================
    // 1. PhysicalPlan -> Operator Tree
    // ==========================================

    ExecutorBuilder builder(ctx_);

    BuiltExecutor executor = builder.Build(plan);

    if (!executor.root) {
        throw std::runtime_error("ExecutorBuilder returned "
                                 "empty operator tree");
    }

    ExecutionResult result;

    result.type = ExecutionResultType::QUERY;

    result.schema = executor.output_schema;

    // ==========================================
    // 2. 初始化整个 Operator Tree
    // ==========================================

    executor.root->Init();

    // ==========================================
    // 3. Pull execution
    // ==========================================

    VectorBatch batch;

    while (true) {
        // 每次 Next 都要求产生一个新的逻辑 batch。
        batch.Reset();

        if (!executor.root->Next(batch)) {
            break;
        }

        const uint32_t active_rows = ActiveRowCount(batch);

        // 我们之前规定：
        //
        // Next() == true
        // 应该尽量保证 batch 非空。
        //
        // 这里保守处理一下。
        if (active_rows == 0) {
            continue;
        }

        result.row_count += active_rows;

        // ======================================
        // 把 batch 交给上层
        // ======================================

        if (consumer) {
            consumer(batch, result.schema);
        }

        // callback 返回之后，
        // 上层不能继续持有 batch.data 的裸引用。
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