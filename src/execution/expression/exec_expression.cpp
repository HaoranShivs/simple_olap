#include "exec_expression.h"

#include <cmath>
#include <stdexcept>
#include <type_traits>

namespace simple_olap {

namespace {

ExecValue PlanLiteralToExec(const PlanLiteralValue &value) {
    if (std::holds_alternative<int32_t>(value)) {
        return std::get<int32_t>(value);
    }
    if (std::holds_alternative<double>(value)) {
        return std::get<double>(value);
    }
    return std::get<std::string>(value);
}

bool IsComparison(PlanBinaryOp op) {
    return op == PlanBinaryOp::EQ || op == PlanBinaryOp::GT || op == PlanBinaryOp::LT;
}

} // namespace

ExecValue ReadExecValue(const ColumnData &column, uint32_t physical_row) {
    if (physical_row >= column.count) {
        throw std::runtime_error("execution: row index out of range");
    }

    switch (column.type) {
    case DataType::INT32:
        return column.data<int32_t>()[physical_row];
    case DataType::INT64:
        return column.data<int64_t>()[physical_row];
    case DataType::FLOAT:
        return column.data<float>()[physical_row];
    case DataType::DOUBLE:
        return column.data<double>()[physical_row];
    case DataType::VARCHAR:
        throw std::runtime_error(
            "execution: VARCHAR vector layout is not defined yet; keep v1 numeric-only");
    default:
        throw std::runtime_error("execution: invalid column type");
    }
}

long double ExecValueAsNumber(const ExecValue &value) {
    return std::visit(
        [](const auto &v) -> long double {
            using T = std::decay_t<decltype(v)>;
            if constexpr (std::is_same_v<T, int32_t> ||
                          std::is_same_v<T, int64_t> ||
                          std::is_same_v<T, float> ||
                          std::is_same_v<T, double>) {
                return static_cast<long double>(v);
            } else if constexpr (std::is_same_v<T, bool>) {
                return v ? 1.0L : 0.0L;
            } else {
                throw std::runtime_error("execution: string is not numeric");
            }
        },
        value);
}

bool ExecValueAsBool(const ExecValue &value) {
    if (std::holds_alternative<bool>(value)) {
        return std::get<bool>(value);
    }
    if (std::holds_alternative<std::string>(value)) {
        return !std::get<std::string>(value).empty();
    }
    return ExecValueAsNumber(value) != 0.0L;
}

ExecValue CastNumber(long double value, DataType type) {
    switch (type) {
    case DataType::INT32:
        return static_cast<int32_t>(value);
    case DataType::INT64:
        return static_cast<int64_t>(value);
    case DataType::FLOAT:
        return static_cast<float>(value);
    case DataType::DOUBLE:
        return static_cast<double>(value);
    default:
        return static_cast<double>(value);
    }
}

void WriteExecValue(ColumnData &column, uint32_t physical_row, const ExecValue &value) {
    if (physical_row >= column.count) {
        throw std::runtime_error("execution: write row index out of range");
    }

    switch (column.type) {
    case DataType::INT32:
        column.mutable_data<int32_t>()[physical_row] =
            static_cast<int32_t>(ExecValueAsNumber(value));
        return;
    case DataType::INT64:
        column.mutable_data<int64_t>()[physical_row] =
            static_cast<int64_t>(ExecValueAsNumber(value));
        return;
    case DataType::FLOAT:
        column.mutable_data<float>()[physical_row] =
            static_cast<float>(ExecValueAsNumber(value));
        return;
    case DataType::DOUBLE:
        column.mutable_data<double>()[physical_row] =
            static_cast<double>(ExecValueAsNumber(value));
        return;
    case DataType::VARCHAR:
        throw std::runtime_error(
            "execution: VARCHAR materialization is not defined in VectorBatch v1");
    default:
        throw std::runtime_error("execution: invalid output type");
    }
}

int CompareExecValues(const ExecValue &lhs, const ExecValue &rhs) {
    if (std::holds_alternative<std::string>(lhs) ||
        std::holds_alternative<std::string>(rhs)) {
        if (!std::holds_alternative<std::string>(lhs) ||
            !std::holds_alternative<std::string>(rhs)) {
            throw std::runtime_error("execution: cannot compare string with numeric value");
        }
        const auto &a = std::get<std::string>(lhs);
        const auto &b = std::get<std::string>(rhs);
        if (a < b) return -1;
        if (a > b) return 1;
        return 0;
    }

    const long double a = ExecValueAsNumber(lhs);
    const long double b = ExecValueAsNumber(rhs);
    if (a < b) return -1;
    if (a > b) return 1;
    return 0;
}

ExecValue ExecColumnRef::Eval(const VectorBatch &batch, uint32_t physical_row) const {
    if (input_slot_ >= batch.columns.size()) {
        throw std::runtime_error("execution: input slot out of range");
    }
    return ReadExecValue(batch.columns[input_slot_], physical_row);
}

ExecValue ExecBinary::Eval(const VectorBatch &batch, uint32_t physical_row) const {
    if (op_ == PlanBinaryOp::AND) {
        const bool left = ExecValueAsBool(left_->Eval(batch, physical_row));
        if (!left) return false;
        return ExecValueAsBool(right_->Eval(batch, physical_row));
    }
    if (op_ == PlanBinaryOp::OR) {
        const bool left = ExecValueAsBool(left_->Eval(batch, physical_row));
        if (left) return true;
        return ExecValueAsBool(right_->Eval(batch, physical_row));
    }

    const ExecValue lhs = left_->Eval(batch, physical_row);
    const ExecValue rhs = right_->Eval(batch, physical_row);

    if (IsComparison(op_)) {
        const int cmp = CompareExecValues(lhs, rhs);
        switch (op_) {
        case PlanBinaryOp::EQ: return cmp == 0;
        case PlanBinaryOp::GT: return cmp > 0;
        case PlanBinaryOp::LT: return cmp < 0;
        default: break;
        }
    }

    const long double a = ExecValueAsNumber(lhs);
    const long double b = ExecValueAsNumber(rhs);
    switch (op_) {
    case PlanBinaryOp::ADD:
        return CastNumber(a + b, GetReturnType());
    case PlanBinaryOp::SUB:
        return CastNumber(a - b, GetReturnType());
    default:
        throw std::runtime_error("execution: unsupported binary operator");
    }
}

ExecExprPtr CompileExecExpr(const PlanExpr &expr, const ExecSchema &input_schema) {
    switch (expr.GetType()) {
    case PlanExpr::Type::COLUMN_REF: {
        const auto &col = static_cast<const PlanColumnRef &>(expr);
        ColumnBinding binding{col.GetTableOid(), col.GetColumnIndex()};
        auto slot = FindInputSlot(input_schema, binding);
        if (!slot.has_value()) {
            throw std::runtime_error(
                "execution: column " + col.ToString() + " not found in child output schema");
        }
        return std::make_unique<ExecColumnRef>(*slot, expr.GetReturnType());
    }
    case PlanExpr::Type::LITERAL: {
        const auto &lit = static_cast<const PlanLiteral &>(expr);
        return std::make_unique<ExecLiteral>(
            PlanLiteralToExec(lit.GetValue()), expr.GetReturnType());
    }
    case PlanExpr::Type::BINARY_OP: {
        const auto &binary = static_cast<const PlanBinaryExpr &>(expr);
        return std::make_unique<ExecBinary>(
            binary.GetOp(),
            CompileExecExpr(binary.GetLeft(), input_schema),
            CompileExecExpr(binary.GetRight(), input_schema),
            expr.GetReturnType());
    }
    case PlanExpr::Type::AGG_FUNC:
        throw std::runtime_error(
            "execution: aggregate expressions are compiled by HashAggregateOperator");
    }
    throw std::runtime_error("execution: unknown plan expression type");
}

} // namespace simple_olap
