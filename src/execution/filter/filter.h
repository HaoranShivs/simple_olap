#pragma once

#include <memory>

#include "../../simd/selection_mask.h"
#include "../expression/exec_expression.h"
#include "../expression/vector_predicate.h"
#include "../operator.h"

namespace simple_olap {

// FilterOperator：把子算子的每个 batch 按谓词过滤。
//
// mask-native 执行模型：
//   - Init() 中把 predicate_ 一次性编译成 VectorPredicate 树；
//     可 SIMD 的比较走 CompareKernels，不可 SIMD 的子树退化为逐行标量。
//   - Next() 热路径直接读写 batch.selection，不存在
//     mask -> selection vector -> mask 的往返。
//   - predicate_ 始终保留：ScalarVectorPredicate 持有其非拥有指针，
//     同时也是编译产物的生命周期宿主。
class FilterOperator final : public Operator {
  public:
    FilterOperator(std::unique_ptr<Operator> child, ExecExprPtr predicate)
        : child_(std::move(child)), predicate_(std::move(predicate)) {}

    void Init() override;
    bool Next(VectorBatch& batch) override;

  private:
    std::unique_ptr<Operator> child_;

    // 永远保留，作为标量兜底与编译产物生命周期的宿主。
    ExecExprPtr predicate_;

    // Init 时尝试编译，只编译一次。
    std::unique_ptr<VectorPredicate> prepared_predicate_;

    // 可复用的 SIMD mask 输出。
    simd::SelectionMask result_mask_;
};

} // namespace simple_olap
