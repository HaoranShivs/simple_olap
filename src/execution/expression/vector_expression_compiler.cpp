#include "vector_expression_compiler.h"

#include <cstdint>
#include <optional>

#include "numeric_value.h"

namespace simple_olap {

namespace {

// 把一个子表达式识别成算术操作数；无法无损映射到 result_type 时返回 nullopt。
//
//   - 列引用：要求列类型恰好等于 result_type（不做隐式类型转换，避免跨类型读取）；
//   - 数值字面量：要求能精确收敛到 result_type（与 VectorPredicate 用同一套规则）。
std::optional<ArithmeticOperand> ClassifyOperand(const ExecExpression& expr, DataType result_type) {
    if (expr.GetType() == ExecExpression::Type::COLUMN_REF) {
        const auto& col = static_cast<const ExecColumnRef&>(expr);
        if (col.GetReturnType() != result_type) {
            return std::nullopt;
        }
        ArithmeticOperand op;
        op.kind = ArithmeticOperand::Kind::COLUMN;
        op.type = result_type;
        op.column_slot = col.GetInputSlot();
        return op;
    }

    if (expr.GetType() == ExecExpression::Type::LITERAL) {
        const auto& lit = static_cast<const ExecLiteral&>(expr);
        NumericConstant constant;
        if (!TryCoerceExact(lit.GetValue(), result_type, constant)) {
            return std::nullopt;
        }
        ArithmeticOperand op;
        op.kind = ArithmeticOperand::Kind::CONSTANT;
        op.type = result_type;
        op.constant = constant;
        return op;
    }

    return std::nullopt;
}

} // namespace

std::unique_ptr<VectorExpression> TryCompileVectorExpression(const ExecExpression& expr) {
    if (expr.GetType() != ExecExpression::Type::BINARY) {
        return nullptr;
    }

    const auto& binary = static_cast<const ExecBinary&>(expr);

    simd::ArithmeticOp op;
    switch (binary.GetOp()) {
    case PlanBinaryOp::ADD:
        op = simd::ArithmeticOp::ADD;
        break;
    case PlanBinaryOp::SUB:
        op = simd::ArithmeticOp::SUB;
        break;
    default:
        return nullptr; // 乘除、比较、逻辑都不在本层处理
    }

    const DataType result_type = expr.GetReturnType();
    if (!IsNumericType(result_type)) {
        return nullptr;
    }

    auto lhs = ClassifyOperand(binary.GetLeft(), result_type);
    if (!lhs.has_value()) {
        return nullptr;
    }
    auto rhs = ClassifyOperand(binary.GetRight(), result_type);
    if (!rhs.has_value()) {
        return nullptr;
    }

    // 两个常量没有 SIMD 价值（也说明这条表达式实际是常量折叠的候选）。
    if (lhs->kind == ArithmeticOperand::Kind::CONSTANT && rhs->kind == ArithmeticOperand::Kind::CONSTANT) {
        return nullptr;
    }

    return std::make_unique<ArithmeticVectorExpression>(op, std::move(*lhs), std::move(*rhs), result_type);
}

} // namespace simple_olap
