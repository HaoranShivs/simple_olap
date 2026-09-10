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

ExecutionResult ParallelExecutionEngine::ExecuteQuery(const PhysicalPlan& plan, const BatchConsumer& consumer) {
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

// ==========================================
// 辅助函数：在算子树中查找 HashAggregateOperator
// ==========================================

namespace {

HashAggregateOperator* FindAggregateOperator(Operator* node) {
    if (node == nullptr) {
        return nullptr;
    }

    if (auto* agg = dynamic_cast<HashAggregateOperator*>(node)) {
        return agg;
    }

    // Filter / Project 都只有一个 child；SeqScan 是叶子。
    // 这里通过类型判断逐层下钻，避免给 Operator 接口加虚函数。
    if (auto* filter = dynamic_cast<FilterOperator*>(node)) {
        return FindAggregateOperator(filter->GetChild());
    }

    if (auto* project = dynamic_cast<ProjectionOperator*>(node)) {
        return FindAggregateOperator(project->GetChild());
    }

    return nullptr;
}

} // namespace

ParallelExecutionEngine::ParallelExecutionEngine(ExecutionContext* ctx) : ctx_(ctx) {
    if (ctx_ == nullptr) {
        throw std::runtime_error("ParallelExecutionEngine requires "
                                 "ExecutionContext");
    }
}

ExecutionResult ParallelExecutionEngine::Execute(const PhysicalPlan& plan, const BatchConsumer& consumer) {
    if (plan.GetType() == PhysicalPlan::Type::HASH_AGGREGATE) {
        // 聚合查询：Aggregate 作为分界线拆成两棵树
        return ExecuteParallelAggregate(plan, consumer);
    }

    // 非聚合查询 / 命令计划：复用单线程路径
    return ExecutionEngine::Execute(plan, consumer);
}

size_t ParallelExecutionEngine::CalculateParallelism(size_t segment_count) const {
    if (ctx_->thread_pool == nullptr) {
        return 1;
    }

    // 并行度受两个因素约束：
    //   - 线程池的线程数（再多任务也只是排队）
    //   - segment 数（每个 worker 至少要分到一个 segment 才有意义）
    const size_t threads = ctx_->thread_pool->thread_count();
    const size_t by_segments = segment_count == 0 ? 1 : segment_count;

    return std::max<size_t>(1, std::min(threads, by_segments));
}

ExecutionResult ParallelExecutionEngine::ExecuteParallelAggregate(const PhysicalPlan& plan,
                                                                  const BatchConsumer& consumer) {
    // ==========================================
    // 1. 找到 Aggregate，拆分 operator 树
    // ==========================================

    const auto& agg_plan = static_cast<const PhysicalHashAggregate&>(plan);

    // 聚合前子计划：Scan -> Filter -> Project（Aggregate 的 child）
    const PhysicalPlan& pre_agg_plan = agg_plan.GetChild();

    // 先构建一次完整算子树，用于：
    //   - 定位 Aggregate 算子（拿 group_exprs / agg_calls / outputs 的编译结果）
    //   - 拿到输出 schema
    ExecutorBuilder builder(ctx_);

    BuiltExecutor executor = builder.Build(plan);

    if (!executor.root) {
        throw std::runtime_error("ExecutorBuilder returned "
                                 "empty operator tree");
    }

    HashAggregateOperator* agg_op = FindAggregateOperator(executor.root.get());

    if (agg_op == nullptr) {
        throw std::runtime_error("ParallelExecutionEngine: HASH_AGGREGATE plan "
                                 "has no aggregate operator");
    }

    ExecutionResult result;

    result.type = ExecutionResultType::QUERY;

    result.schema = executor.output_schema;

    // ==========================================
    // 2. 重置共享 segment 分配器（多线程开始前的准备）
    // ==========================================
    // ScanCursor 初始 segment_id 是 uint32 最大值（魔法值），
    // 各 worker 首次 Scan 时从 TableStorage 的共享 SegmentSource
    // 领取起始 segment，之后每次 segment 读完也从分配器领取下一个，
    // 因此各 worker 天然扫描互不重叠的 segment 集合。

    const TableCatalogEntry* scan_entry = nullptr;

    // 找到聚合前子树中的 scan 表，用于重置其 segment 分配器
    {
        const PhysicalPlan* node = &pre_agg_plan;
        while (node->GetType() != PhysicalPlan::Type::SEQ_SCAN) {
            switch (node->GetType()) {
            case PhysicalPlan::Type::FILTER:
                node = &static_cast<const PhysicalFilter&>(*node).GetChild();
                break;
            case PhysicalPlan::Type::PROJECT:
                node = &static_cast<const PhysicalProject&>(*node).GetChild();
                break;
            default:
                throw std::runtime_error("ParallelExecutionEngine: unsupported plan node "
                                         "below aggregate");
            }
        }

        const auto& scan_plan = static_cast<const PhysicalSeqScan&>(*node);

        scan_entry = ctx_->catalog->GetTable(scan_plan.GetTableOid());
        if (scan_entry == nullptr) {
            throw std::runtime_error("ParallelExecutionEngine: scan table not found");
        }

        auto storage = ctx_->storage_manager->GetTable(scan_plan.GetTableOid(), scan_entry->schema);
        if (storage == nullptr) {
            throw std::runtime_error("ParallelExecutionEngine: table storage not found");
        }

        storage->ResetSegmentAllocator();
    }

    // ==========================================
    // 3. 聚合前阶段（多线程）：每个 worker 构建独立的
    //    「聚合前子树 + 部分 Aggregate」，产出部分 group 表
    // ==========================================

    const size_t parallelism = CalculateParallelism(
        ctx_->storage_manager->GetTable(scan_entry->table_oid(), scan_entry->schema)->segment_count());

    std::vector<std::future<HashAggregateOperator::GroupTable>> futures;

    futures.reserve(parallelism);

    for (size_t i = 0; i < parallelism; ++i) {
        futures.emplace_back(
            ctx_->thread_pool->Submit([&agg_plan, &pre_agg_plan, this]() -> HashAggregateOperator::GroupTable {
                // 每个 worker 独立构建一棵算子树：
                //   聚合前子树（Scan -> Filter -> Project）+ 部分 Aggregate
                // 树内所有对象都是 worker 私有的，无共享可变状态；
                // 唯一的共享点是 TableStorage 的 segment 分配器（原子）与
                // reader 缓存（互斥锁保护）。
                ExecutorBuilder worker_builder(ctx_);

                BuiltExecutor worker_executor = worker_builder.Build(agg_plan);

                HashAggregateOperator* worker_agg = FindAggregateOperator(worker_executor.root.get());

                if (worker_agg == nullptr) {
                    throw std::runtime_error("ParallelExecutionEngine: worker built "
                                             "no aggregate operator");
                }

                // 只消费本 worker 分到的 segment，形成部分 group 表
                worker_agg->ComputePartialGroups();

                return worker_agg->TakeGroups();
            }));
    }

    // ==========================================
    // 4. 聚合后阶段（单线程）：合并所有部分 group 表
    // ==========================================

    for (auto& future : futures) {
        HashAggregateOperator::GroupTable partial = future.get();

        agg_op->MergeGroups(std::move(partial));
    }

    // ==========================================
    // 5. Finalize：把合并后的 group 表 finalize 并流式产出
    // ==========================================
    // SetExternalGroups 让 Next() 跳过 ConsumeAll（输入已消费完），
    // 直接进入 emit 阶段。

    agg_op->SetExternalGroups();

    VectorBatch batch;

    while (true) {
        batch.Reset();

        if (!agg_op->Next(batch)) {
            break;
        }

        const uint32_t active_rows = ActiveRowCount(batch);

        if (active_rows == 0) {
            continue;
        }

        result.row_count += active_rows;

        if (consumer) {
            consumer(batch, result.schema);
        }
    }

    return result;
}

} // namespace simple_olap