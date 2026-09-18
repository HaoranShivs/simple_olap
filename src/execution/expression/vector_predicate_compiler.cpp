#include "vector_predicate_compiler.h"

#include <cstdint>

// 精确收敛工具（IsNumericType / TryCoerceExact / CoerceFrom*）与
// vector_expression_compiler 共用同一份实现。
#include "numeric_value.h"

namespace simple_olap {

namespace {

enum class OperandKind : uint8_t {
    COLUMN,
    CONSTANT,
    OTHER,
};

struct StaticOperand {
    OperandKind kind = OperandKind::OTHER;
    uint32_t column_slot = 0;
    DataType type = DataType::INVALID;
    const ExecValue* literal = nullptr; // 仅 CONSTANT 有效
};

StaticOperand Classify(const ExecExpression& expr) {
    StaticOperand op;
    if (expr.GetType() == ExecExpression::Type::COLUMN_REF) {
        const auto& col = static_cast<const ExecColumnRef&>(expr);
        op.kind = OperandKind::COLUMN;
        op.column_slot = col.GetInputSlot();
        op.type = col.GetReturnType();
    } else if (expr.GetType() == ExecExpression::Type::LITERAL) {
        const auto& lit = static_cast<const ExecLiteral&>(expr);
        op.kind = OperandKind::CONSTANT;
        op.type = lit.GetReturnType();
        op.literal = &lit.GetValue();
    }
    return op;
}

// 尝试编译数值比较；无法保证语义等价时返回 nullptr（由上层退化为标量）。
std::unique_ptr<VectorPredicate> TryCompileNumericCompare(CmpOp op, const ExecExpression& left,
                                                          const ExecExpression& right) {
    const StaticOperand lhs = Classify(left);
    const StaticOperand rhs = Classify(right);

    // Column OP Column：要求两侧同类型且为数值类型。
    if (lhs.kind == OperandKind::COLUMN && rhs.kind == OperandKind::COLUMN) {
        if (lhs.type != rhs.type || !IsNumericType(lhs.type)) {
            return nullptr;
        }
        CompareOperand a;
        a.kind = CompareOperand::Kind::COLUMN;
        a.type = lhs.type;
        a.column_slot = lhs.column_slot;

        CompareOperand b;
        b.kind = CompareOperand::Kind::COLUMN;
        b.type = rhs.type;
        b.column_slot = rhs.column_slot;

        return std::make_unique<NumericComparePredicate>(op, a, b);
    }

    // Column OP Constant
    if (lhs.kind == OperandKind::COLUMN && rhs.kind == OperandKind::CONSTANT) {
        if (!IsNumericType(lhs.type)) {
            return nullptr;
        }
        NumericConstant constant;
        if (!TryCoerceExact(*rhs.literal, lhs.type, constant)) {
            return nullptr;
        }
        CompareOperand a;
        a.kind = CompareOperand::Kind::COLUMN;
        a.type = lhs.type;
        a.column_slot = lhs.column_slot;

        CompareOperand b;
        b.kind = CompareOperand::Kind::CONSTANT;
        b.type = lhs.type;
        b.constant = constant;

        return std::make_unique<NumericComparePredicate>(op, a, b);
    }

    // Constant OP Column：反转比较符，规范化为 column OP constant。
    if (lhs.kind == OperandKind::CONSTANT && rhs.kind == OperandKind::COLUMN) {
        if (!IsNumericType(rhs.type)) {
            return nullptr;
        }
        NumericConstant constant;
        if (!TryCoerceExact(*lhs.literal, rhs.type, constant)) {
            return nullptr;
        }
        CompareOperand a;
        a.kind = CompareOperand::Kind::COLUMN;
        a.type = rhs.type;
        a.column_slot = rhs.column_slot;

        CompareOperand b;
        b.kind = CompareOperand::Kind::CONSTANT;
        b.type = rhs.type;
        b.constant = constant;

        return std::make_unique<NumericComparePredicate>(ReverseCmpOp(op), a, b);
    }

    return nullptr;
}

} // namespace

std::unique_ptr<VectorPredicate> CompileVectorPredicate(const ExecExpression& expr) {
    if (expr.GetType() == ExecExpression::Type::BINARY) {
        const auto& binary = static_cast<const ExecBinary&>(expr);
        const PlanBinaryOp op = binary.GetOp();

        if (IsPlanComparison(op)) {
            if (auto numeric = TryCompileNumericCompare(ToCmpOp(op), binary.GetLeft(), binary.GetRight())) {
                return numeric;
            }
        } else if (op == PlanBinaryOp::AND || op == PlanBinaryOp::OR) {
            auto lhs = CompileVectorPredicate(binary.GetLeft());
            auto rhs = CompileVectorPredicate(binary.GetRight());
            const auto logical =
                (op == PlanBinaryOp::AND) ? LogicalVectorPredicate::Op::AND : LogicalVectorPredicate::Op::OR;
            return std::make_unique<LogicalVectorPredicate>(logical, std::move(lhs), std::move(rhs));
        }
    }

    return std::make_unique<ScalarVectorPredicate>(&expr);
}

} // namespace simple_olap
