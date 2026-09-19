#pragma once

#include <cstdint>
#include <utility>
#include <vector>

#include "../../simd/arithmetic_kernel.h"
#include "../../type.h"
#include "exec_expression.h"
#include "numeric_value.h"

namespace simple_olap {

// ============================================================
// VectorExpression：与逐行 ExecExpression::Eval 平行的「按整列求值」体系。
// ============================================================
//
// 设计要点：
//   - 不替换逐行接口，也不修改 ExecExpression；两者共存。
//   - EvaluateDense 一次性算出整批结果，避免每个 active row 一次虚函数调用 +
//     一次 variant 构造；热路径只做类型已绑定好的内核调用。
//   - 输入可以是 dense，也可以是 gather 后的 dense 临时 batch（sparse 路径）：
//     ProjectionOperator 先用 CollectInputSlots 收集依赖列，
//     mask -> typed gather -> dense temporary -> 本接口。
//   - 语义等价由「精确收敛」保证：算术常量必须在 Init 阶段无损收敛到列的
//     存储类型，否则编译器直接不生成 VectorExpression（退回标量路径）。
class VectorExpression {
  public:
    virtual ~VectorExpression() = default;

    virtual DataType result_type() const = 0;

    // 输入必须是 dense（selection.IsAll()），count 必须等于物理行数。
    // output 必须已按 count 行分配（Resize(count)）且 type == result_type()。
    // 输出同样为 dense。
    virtual void EvaluateDense(const VectorBatch& input, ColumnData& output, uint32_t count) const = 0;

    // 收集本表达式直接读取的 batch 列 slot（可能重复）。
    // ProjectionOperator::Init() 调用一次，去重后作为 sparse gather 的列集合。
    virtual void CollectInputSlots(std::vector<uint32_t>& slots) const = 0;
};

// 算术操作数：要么是 batch 中的一列，要么是一个已经绑定好类型的常量。
struct ArithmeticOperand {
    enum class Kind : uint8_t {
        COLUMN,
        CONSTANT,
    };

    Kind kind = Kind::CONSTANT;
    DataType type = DataType::INVALID;
    uint32_t column_slot = 0;
    NumericConstant constant{int32_t{0}};
};

// ---------- 算术表达式 ----------
//
// 覆盖：Column +/- Column、Column +/- Constant、Constant +/- Column。
// 调用方（编译器）保证两侧类型都已收敛到 result_type()。
class ArithmeticVectorExpression final : public VectorExpression {
  public:
    ArithmeticVectorExpression(simd::ArithmeticOp op, ArithmeticOperand lhs, ArithmeticOperand rhs, DataType type)
        : op_(op), lhs_(std::move(lhs)), rhs_(std::move(rhs)), type_(type) {}

    DataType result_type() const override {
        return type_;
    }

    void EvaluateDense(const VectorBatch& input, ColumnData& output, uint32_t count) const override;

    void CollectInputSlots(std::vector<uint32_t>& slots) const override {
        if (lhs_.kind == ArithmeticOperand::Kind::COLUMN) {
            slots.push_back(lhs_.column_slot);
        }
        if (rhs_.kind == ArithmeticOperand::Kind::COLUMN) {
            slots.push_back(rhs_.column_slot);
        }
    }

  private:
    simd::ArithmeticOp op_;
    ArithmeticOperand lhs_;
    ArithmeticOperand rhs_;
    DataType type_ = DataType::INVALID;
};

} // namespace simple_olap
