#pragma once

#include <memory>
#include <vector>

#include "../operator.h"
#include "hash_aggregate_state.h"

namespace simple_olap {

// Hash 聚合算子外壳：真正的聚合状态与算法在 HashAggregateState 中。
//
// 串行执行路径：
//   while (child_->Next(input_)) state_.Consume(input_);
//   return state_.NextResult(output);
//
// 并行执行路径（ParallelExecutor）不使用本算子消费输入，
// 而是直接为每个 worker 构造独立的 HashAggregateState。
class HashAggregateOperator final : public Operator {
  public:
    HashAggregateOperator(std::unique_ptr<Operator> child, std::vector<ExecExprPtr> group_exprs,
                          std::vector<AggCallSpec> agg_calls, std::vector<AggregateOutputSpec> outputs)
        : child_(std::move(child)), group_exprs_(std::move(group_exprs)), agg_calls_(std::move(agg_calls)),
          outputs_(std::move(outputs)), state_(&group_exprs_, &agg_calls_) {
        state_.set_outputs(&outputs_);
    }

    void Init() override;
    bool Next(VectorBatch& output) override;

  private:
    std::unique_ptr<Operator> child_;
    std::vector<ExecExprPtr> group_exprs_;
    std::vector<AggCallSpec> agg_calls_;
    std::vector<AggregateOutputSpec> outputs_;

    HashAggregateState state_;
    VectorBatch input_{true};
    bool consumed_ = false;
};

} // namespace simple_olap
