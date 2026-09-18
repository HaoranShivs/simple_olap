#pragma once

#include <cstdint>
#include <memory>

#include "../../simd/selection_mask.h"
#include "../../type.h"
#include "exec_expression.h"
#include "numeric_value.h"

namespace simple_olap {

// ============================================================
// VectorPredicate：与逐行 ExecExpression::Eval 平行的「按 mask 求值」体系。
// ============================================================
//
// 设计要点：
//   - 不替换逐行接口，也不修改 ExecExpression；两者共存。
//   - Evaluate 以 SelectionMask 为单位工作，一次覆盖整个 batch，
//     避免每个 active row 一次虚函数调用。
//   - input_mask 是必须的：它承载上游（Storage 下推 / 上级 Filter）
//     已经确定的有效行集合，同时让 AND / OR 能在行级短路。
//
// 强不变式（所有实现必须保证）：
//   output_mask ⊆ input_mask
//   即：任何未在 input_mask 中置位的行，都不会出现在 output_mask 中。
//   这样 LogicalVectorPredicate 才能安全地做位运算组合。
class VectorPredicate {
  public:
    virtual ~VectorPredicate() = default;

    // 在 [0, physical_count) 范围内求值。
    //   - physical_count 必须等于 batch 的物理行数。
    //   - input_mask.row_count() 必须等于 physical_count。
    //   - output_mask 会被完整覆盖（先 SetNone(physical_count) 再写位）。
    virtual void Evaluate(const VectorBatch& batch, uint32_t physical_count, const simd::SelectionMask& input_mask,
                          simd::SelectionMask& output_mask) const = 0;
};

// 数值常量 NumericConstant：编译器在 Init 阶段一次性把 SQL 字面量精确收敛到
// 列的存储类型（定义见 numeric_value.h）。

// 比较操作数：要么是 batch 中的一列，要么是一个已经绑定好类型的常量。
struct CompareOperand {
    enum class Kind : uint8_t {
        COLUMN,
        CONSTANT,
    };

    Kind kind = Kind::CONSTANT;
    DataType type = DataType::INVALID;
    uint32_t column_slot = 0;
    NumericConstant constant{int32_t{0}};
};

// 反转比较运算符（用于把 `constant OP column` 规范化成 `column ReverseOp constant`）。
inline CmpOp ReverseCmpOp(CmpOp op) {
    switch (op) {
    case CmpOp::LT:
        return CmpOp::GT;
    case CmpOp::LE:
        return CmpOp::GE;
    case CmpOp::GT:
        return CmpOp::LT;
    case CmpOp::GE:
        return CmpOp::LE;
    case CmpOp::EQ:
    case CmpOp::NE:
        return op;
    }
    return op;
}

// ---------- 数值比较谓词 ----------
//
// 复用已通过验证的 simd::CompareKernels（scalar / AVX2 运行时派发）。
// 覆盖：Column OP Constant、Column OP Column。
// 调用方（编译器）保证两侧类型一致且为数值类型。
class NumericComparePredicate final : public VectorPredicate {
  public:
    NumericComparePredicate(CmpOp op, CompareOperand lhs, CompareOperand rhs)
        : op_(op), lhs_(std::move(lhs)), rhs_(std::move(rhs)), type_(lhs_.type) {}

    void Evaluate(const VectorBatch& batch, uint32_t physical_count, const simd::SelectionMask& input_mask,
                  simd::SelectionMask& output_mask) const override;

  private:
    CmpOp op_;
    CompareOperand lhs_;
    CompareOperand rhs_;
    DataType type_ = DataType::INVALID;
};

// ---------- 逻辑谓词 ----------
//
// AND：左侧结果为空即可短路。
// OR ：右侧只在「左侧为假」的行上求值（input AND NOT lhs）。
class LogicalVectorPredicate final : public VectorPredicate {
  public:
    enum class Op : uint8_t {
        AND,
        OR,
    };

    LogicalVectorPredicate(Op op, std::unique_ptr<VectorPredicate> lhs, std::unique_ptr<VectorPredicate> rhs)
        : op_(op), lhs_(std::move(lhs)), rhs_(std::move(rhs)) {}

    void Evaluate(const VectorBatch& batch, uint32_t physical_count, const simd::SelectionMask& input_mask,
                  simd::SelectionMask& output_mask) const override;

  private:
    Op op_;
    std::unique_ptr<VectorPredicate> lhs_;
    std::unique_ptr<VectorPredicate> rhs_;
};

// ---------- 标量兜底谓词 ----------
//
// 直接复用逐行 ExecExpression::Eval + ExecValueAsBool，语义与旧 Filter 完全一致。
// 持有的是非拥有指针，生命周期由 FilterOperator 的 predicate_ 保证。
// 用于：VARCHAR 比较、混合数值类型、嵌套算术、任意无法 SIMD 的子树。
class ScalarVectorPredicate final : public VectorPredicate {
  public:
    explicit ScalarVectorPredicate(const ExecExpression* expr) : expr_(expr) {}

    void Evaluate(const VectorBatch& batch, uint32_t physical_count, const simd::SelectionMask& input_mask,
                  simd::SelectionMask& output_mask) const override;

  private:
    const ExecExpression* expr_ = nullptr;
};

} // namespace simple_olap
