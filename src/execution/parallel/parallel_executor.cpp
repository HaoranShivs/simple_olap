#include "parallel_executor.h"

#include <algorithm>
#include <atomic>
#include <exception>
#include <future>
#include <stdexcept>
#include <string>
#include <vector>

#include "../../catalog/catalog.h"
#include "../../parallel/queue/bounded_blocking_queue.h"
#include "../../storage/scan/parallel_scan_session.h"
#include "../../storage/storage_manager.h"
#include "../../storage/table/table_storage.h"
#include "../aggregate/hash_aggregate_state.h"
#include "../batch_utils.h"
#include "../executor_builder.h"

namespace simple_olap {

namespace {

// 在 SeqScan / Filter / Project 单输入链上找到唯一的叶子 SeqScan。
// CanParallelizeAggregate() 已保证链形状合法。
const PhysicalSeqScan* FindSeqScanPlan(const PhysicalPlan& plan) {
    switch (plan.GetType()) {
    case PhysicalPlan::Type::SEQ_SCAN:
        return &static_cast<const PhysicalSeqScan&>(plan);

    case PhysicalPlan::Type::FILTER:
        return FindSeqScanPlan(static_cast<const PhysicalFilter&>(plan).GetChild());

    case PhysicalPlan::Type::PROJECT:
        return FindSeqScanPlan(static_cast<const PhysicalProject&>(plan).GetChild());

    default:
        return nullptr;
    }
}

} // namespace

bool CanParallelizeAggregate(const PhysicalPlan& plan) {
    // root 为 HashAggregate：从它的 child 开始检查链；
    // root 本身是 SEQ_SCAN / FILTER / PROJECT：直接从 root 开始。
    const PhysicalPlan* node = &plan;
    if (plan.GetType() == PhysicalPlan::Type::HASH_AGGREGATE) {
        node = &static_cast<const PhysicalHashAggregate&>(plan).GetChild();
    }

    // node 以下：SEQ_SCAN / FILTER / PROJECT 构成的单输入链，
    // 链上只有一个 SeqScan（即叶子为 SEQ_SCAN）
    for (;;) {
        switch (node->GetType()) {
        case PhysicalPlan::Type::SEQ_SCAN:
            return true;

        case PhysicalPlan::Type::FILTER:
            node = &static_cast<const PhysicalFilter&>(*node).GetChild();
            break;

        case PhysicalPlan::Type::PROJECT:
            node = &static_cast<const PhysicalProject&>(*node).GetChild();
            break;

        default:
            return false;
        }
    }
}

ParallelExecutor::ParallelExecutor(ExecutionContext* ctx, ThreadPool* compute_pool, ParallelConfig config)
    : ctx_(ctx), compute_pool_(compute_pool), config_(config) {}

ExecutionResult ParallelExecutor::ExecuteAggregate(const PhysicalHashAggregate& plan, const BatchConsumer& consumer) {
    if (ctx_ == nullptr || ctx_->catalog == nullptr || ctx_->storage_manager == nullptr) {
        throw std::runtime_error("ParallelExecutor: missing execution context");
    }
    if (compute_pool_ == nullptr) {
        throw std::runtime_error("ParallelExecutor: missing compute thread pool");
    }

    // 1. 取 aggregate 的 child plan（调用方已保证 CanParallelizeAggregate(plan)）
    const PhysicalPlan& child_plan = plan.GetChild();

    // 2. 找到 child 链上唯一的 SeqScan
    const PhysicalSeqScan* scan_plan = FindSeqScanPlan(child_plan);
    if (scan_plan == nullptr) {
        throw std::runtime_error("ParallelExecutor: aggregate child pipeline has no SeqScan");
    }

    ExecutorBuilder builder(ctx_);

    // 3. 复用串行路径的 PhysicalSeqScan -> ScanOptions 转换
    BuiltScanSpec scan_spec = builder.BuildScanSpec(*scan_plan);

    const TableCatalogEntry* entry = ctx_->catalog->GetTable(scan_spec.table_id);
    if (entry == nullptr) {
        throw std::runtime_error("ParallelExecutor: table not found: " + std::to_string(scan_spec.table_id));
    }

    auto storage = ctx_->storage_manager->GetTable(scan_spec.table_id, entry->schema);
    if (storage == nullptr) {
        throw std::runtime_error("ParallelExecutor: table storage not found: " + std::to_string(scan_spec.table_id));
    }

    // 4. 打开并行扫描：TableStorage::CreateParallelScan(...) -> ParallelScanSession::Start()
    std::shared_ptr<BatchStream> stream =
        storage->CreateParallelScan(scan_spec.options, config_.scan_threads, config_.batch_queue_capacity);
    std::static_pointer_cast<ParallelScanSession>(stream)->Start();

    // 5. 编译聚合 spec：只编译一次，所有 worker 只读共享
    //    （ExecExpression::Eval 是 const 且无缓存，跨线程共享安全）。
    //    这里先借用一次普通构建，仅为拿到 child 的输出 schema。
    BuiltExecutor probe = builder.Build(child_plan);
    BuiltAggregateSpec spec = builder.BuildAggregateSpec(plan, probe.output_schema);

    // 6. 为每条 pipeline 提交一个计算任务，得到 future<HashAggregateState>
    const size_t worker_count = std::max<size_t>(1, std::min(config_.compute_threads, compute_pool_->thread_count()));

    const ExecutorBuildOptions build_options{stream};

    std::vector<std::future<HashAggregateState>> futures;
    futures.reserve(worker_count);

    try {
        for (size_t i = 0; i < worker_count; ++i) {
            futures.push_back(
                compute_pool_->Submit([&builder, &child_plan, &build_options, &spec]() -> HashAggregateState {
                    // 每条 pipeline：SeqScan(共享 BatchStream) -> Filter -> Projection，
                    // 聚合由 worker 本地的 HashAggregateState::Consume() 完成
                    BuiltExecutor pipeline = builder.Build(child_plan, build_options);
                    pipeline.root->Init();

                    HashAggregateState local(&spec.group_exprs, &spec.agg_calls);
                    local.set_outputs(&spec.outputs);
                    // 无 GROUP BY：即使本 worker 没分到数据也要产出全局空组
                    local.EnsureGlobalGroup();

                    VectorBatch batch;
                    while (pipeline.root->Next(batch)) {
                        if (ActiveRowCount(batch) > 0) {
                            local.Consume(batch);
                        }
                        batch.Reset();
                    }

                    return local;
                }));
        }
    } catch (...) {
        // 提交失败：取消扫描（唤醒 scan worker），
        // 并等待已提交任务结束，避免它们引用即将销毁的栈对象
        std::exception_ptr submit_error = std::current_exception();
        stream->Cancel();
        for (auto& future : futures) {
            try {
                future.get();
            } catch (...) {
            }
        }
        std::rethrow_exception(submit_error);
    }

    // 7. 收集 future 并 Merge 进 global_state。
    //    即使某个 worker 抛异常，也要把所有 future 收完再上抛，
    //    否则 worker 线程可能仍引用本函数栈上的 spec。
    HashAggregateState global(&spec.group_exprs, &spec.agg_calls);
    global.set_outputs(&spec.outputs);
    global.EnsureGlobalGroup();

    std::exception_ptr error = nullptr;
    for (auto& future : futures) {
        try {
            global.Merge(future.get());
        } catch (...) {
            if (error == nullptr) {
                error = std::current_exception();
            }
        }
    }

    if (error != nullptr) {
        stream->Cancel();
        std::rethrow_exception(error);
    }

    // 8. Finalize 并分批输出给 consumer
    ExecutionResult result;
    result.type = ExecutionResultType::QUERY;
    result.schema = spec.output_schema;

    VectorBatch output;
    while (global.NextResult(output)) {
        const uint32_t active_rows = ActiveRowCount(output);
        if (active_rows == 0) {
            continue;
        }

        result.row_count += active_rows;

        if (consumer) {
            consumer(output, result.schema);
        }
    }

    return result;
}

ExecutionResult ParallelExecutor::ExecuteQuery(const PhysicalPlan& plan, const BatchConsumer& consumer) {
    if (ctx_ == nullptr || ctx_->catalog == nullptr || ctx_->storage_manager == nullptr) {
        throw std::runtime_error("ParallelExecutor: missing execution context");
    }
    if (compute_pool_ == nullptr) {
        throw std::runtime_error("ParallelExecutor: missing compute thread pool");
    }

    // 1. 找到查询链上唯一的 SeqScan（调用方已保证 CanParallelizeAggregate(plan)）
    const PhysicalSeqScan* scan_plan = FindSeqScanPlan(plan);
    if (scan_plan == nullptr) {
        throw std::runtime_error("ParallelExecutor: query pipeline has no SeqScan");
    }

    ExecutorBuilder builder(ctx_);

    // 2. 复用串行路径的 PhysicalSeqScan -> ScanOptions 转换
    BuiltScanSpec scan_spec = builder.BuildScanSpec(*scan_plan);

    const TableCatalogEntry* entry = ctx_->catalog->GetTable(scan_spec.table_id);
    if (entry == nullptr) {
        throw std::runtime_error("ParallelExecutor: table not found: " + std::to_string(scan_spec.table_id));
    }

    auto storage = ctx_->storage_manager->GetTable(scan_spec.table_id, entry->schema);
    if (storage == nullptr) {
        throw std::runtime_error("ParallelExecutor: table storage not found: " + std::to_string(scan_spec.table_id));
    }

    // 3. 打开并行扫描：scan worker -> 有界队列 -> BatchStream::Next()
    std::shared_ptr<BatchStream> stream =
        storage->CreateParallelScan(scan_spec.options, config_.scan_threads, config_.batch_queue_capacity);
    std::static_pointer_cast<ParallelScanSession>(stream)->Start();

    // 4. 普通构建一次仅为拿到输出 schema（不 Init，不访问存储）。
    //    每个 worker 会用同一个 plan 重建等价的 pipeline。
    BuiltExecutor probe = builder.Build(plan);
    const ExecSchema output_schema = probe.output_schema;

    // 5. 每条 pipeline：SeqScan(共享 BatchStream) -> Filter -> Projection，
    //    处理后的结果批次推入结果队列，由调用线程消费。
    const size_t worker_count = std::max<size_t>(1, std::min(config_.compute_threads, compute_pool_->thread_count()));

    const ExecutorBuildOptions build_options{stream};

    BoundedBlockingQueue<VectorBatch> results(config_.batch_queue_capacity);
    std::atomic<size_t> active_workers{worker_count};

    std::vector<std::future<void>> futures;
    futures.reserve(worker_count);

    try {
        for (size_t i = 0; i < worker_count; ++i) {
            futures.push_back(
                compute_pool_->Submit([&builder, &plan, &build_options, &results, &active_workers]() -> void {
                    try {
                        BuiltExecutor pipeline = builder.Build(plan, build_options);
                        pipeline.root->Init();

                        VectorBatch batch;
                        while (pipeline.root->Next(batch)) {
                            if (ActiveRowCount(batch) > 0) {
                                // 队列满时阻塞（背压）；Cancel/Abort 后 Push 返回 false
                                if (!results.Push(std::move(batch))) {
                                    return;
                                }
                            }
                            batch = VectorBatch{};
                        }
                    } catch (...) {
                        // 处理异常：唤醒消费侧，并唤醒其他阻塞在 Push 上的 worker
                        results.Abort(std::current_exception());
                        return;
                    }

                    // 最后一个退出的 worker 关闭结果队列（正常 EOF）
                    if (active_workers.fetch_sub(1, std::memory_order_acq_rel) == 1) {
                        results.Close();
                    }
                }));
        }
    } catch (...) {
        // 提交失败：取消扫描与结果队列，并等待已提交任务结束，
        // 避免它们继续引用本函数栈上的对象
        std::exception_ptr submit_error = std::current_exception();
        stream->Cancel();
        results.Cancel();
        for (auto& future : futures) {
            try {
                future.get();
            } catch (...) {
            }
        }
        std::rethrow_exception(submit_error);
    }

    // 6. 消费结果批次：计数并交给上层
    ExecutionResult result;
    result.type = ExecutionResultType::QUERY;
    result.schema = output_schema;

    VectorBatch output;
    while (results.Pop(output)) {
        const uint32_t active_rows = ActiveRowCount(output);
        if (active_rows == 0) {
            output = VectorBatch{};
            continue;
        }

        result.row_count += active_rows;

        if (consumer) {
            consumer(output, result.schema);
        }
        output = VectorBatch{};
    }

    // 7. 收集异常，并 join 所有 worker（它们在读取本栈上的 builder/plan/results）
    std::exception_ptr error = results.Error();
    for (auto& future : futures) {
        try {
            future.get();
        } catch (...) {
            if (error == nullptr) {
                error = std::current_exception();
            }
        }
    }

    if (error != nullptr) {
        stream->Cancel();
        std::rethrow_exception(error);
    }

    return result;
}

} // namespace simple_olap
