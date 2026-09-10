#pragma once

#include "../../parallel/parallel_config.h"
#include "../../parallel/thread_pool/thread_pool.h"
#include "../../planner/physical_plan/physical_plan.h"
#include "../execution_context.h"
#include "../execution_engine.h"

namespace simple_olap {

// 判断一个物理计划是否可以走并行执行路径。
//
// 两种形状本质相同 —— 都是「单输入算子树 + 唯一的 SeqScan 叶子」：
//
//   HashAggregate                    Projection? / Filter?
//         |                                  |
//   Projection? / Filter?                 SeqScan
//         |
//   SeqScan
//
// 要求：
//   - root 为 HASH_AGGREGATE；或 root 本身是 SEQ_SCAN / FILTER / PROJECT 链的一环
//   - 链上只有一个 SeqScan（叶子）
bool CanParallelizeAggregate(const PhysicalPlan& plan);

// 并行聚合执行器。
//
// 运行结构：
//
//   Storage Stage（ParallelScanSession 内部 scan 线程）
//        |  BoundedBatchQueue
//        v
//   Compute Stage（compute_pool 线程，每条 pipeline：
//        SeqScan(BatchStream) -> Filter -> Projection -> HashAggregateState::Consume）
//        |  future<HashAggregateState>
//        v
//   Coordinator（调用 ExecuteAggregate 的线程）：Merge -> Finalize -> consumer
//
// submit / future / get / Merge 全部限制在 ExecuteAggregate() 函数作用域内。
class ParallelExecutor {
  public:
    ParallelExecutor(ExecutionContext* ctx, ThreadPool* compute_pool, ParallelConfig config = {});

    // 执行并行聚合查询（要求 CanParallelizeAggregate(plan) == true）
    ExecutionResult ExecuteAggregate(const PhysicalHashAggregate& plan, const BatchConsumer& consumer);

    // 执行并行非聚合查询（要求 CanParallelizeAggregate(plan) == true，
    // 且 root 不是 HASH_AGGREGATE）。worker 从共享 BatchStream 拉取批次，
    // 经 Filter / Projection 处理后将结果批次推入结果队列，由调用线程消费。
    ExecutionResult ExecuteQuery(const PhysicalPlan& plan, const BatchConsumer& consumer);

  private:
    ExecutionContext* ctx_ = nullptr;
    ThreadPool* compute_pool_ = nullptr;
    ParallelConfig config_;
};

} // namespace simple_olap
