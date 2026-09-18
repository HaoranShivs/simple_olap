#pragma once

#include <memory>
#include <vector>

#include "../../simd/selection_mask.h"
#include "../expression/exec_expression.h"
#include "../expression/vector_predicate.h"
#include "../operator.h"

namespace simple_olap {

// FilterOperator：把子算子的每个 batch 按谓词过滤。
//
// 混合执行模型：
//   - Init() 中把 predicate_ 一次性编译成 VectorPredicate 树；
//     可 SIMD 的比较走 CompareKernels，不可 SIMD 的子树退化为逐行标量。
//   - Next() 热路径只做 SelectionMask 运算，不再逐 active row 虚调用 Eval()。
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

    // 可复用的 sel_vector 输出缓冲。
    std::vector<uint32_t> selection_;

    // 可复用的 SIMD mask。
    simd::SelectionMask input_mask_;
    simd::SelectionMask result_mask_;
};

} // namespace simple_olap
