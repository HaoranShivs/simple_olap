#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <variant>

#include "../../planner/plan_expression.h"
#include "../schema.h"
#include "../vector/vector.h"

namespace simple_olap {

using ExecValue = std::variant<int32_t, int64_t, float, double, std::string, bool>;

ExecValue ReadExecValue(const ColumnData &column, uint32_t physical_row);
void WriteExecValue(ColumnData &column, uint32_t physical_row, const ExecValue &value);
bool ExecValueAsBool(const ExecValue &value);
long double ExecValueAsNumber(const ExecValue &value);
ExecValue CastNumber(long double value, DataType type);
int CompareExecValues(const ExecValue &lhs, const ExecValue &rhs);

class ExecExpression {
public:
    enum class Type : uint8_t {
        COLUMN_REF,
        LITERAL,
        BINARY,
    };

    ExecExpression(Type type, DataType return_type)
        : type_(type), return_type_(return_type) {}
    virtual ~ExecExpression() = default;

    Type GetType() const { return type_; }
    DataType GetReturnType() const { return return_type_; }

    virtual ExecValue Eval(const VectorBatch &batch, uint32_t physical_row) const = 0;

private:
    Type type_;
    DataType return_type_;
};

using ExecExprPtr = std::unique_ptr<ExecExpression>;

class ExecColumnRef final : public ExecExpression {
public:
    ExecColumnRef(uint32_t input_slot, DataType type)
        : ExecExpression(Type::COLUMN_REF, type), input_slot_(input_slot) {}

    uint32_t GetInputSlot() const { return input_slot_; }
    ExecValue Eval(const VectorBatch &batch, uint32_t physical_row) const override;

private:
    uint32_t input_slot_;
};

class ExecLiteral final : public ExecExpression {
public:
    ExecLiteral(ExecValue value, DataType type)
        : ExecExpression(Type::LITERAL, type), value_(std::move(value)) {}

    ExecValue Eval(const VectorBatch &, uint32_t) const override { return value_; }

private:
    ExecValue value_;
};

class ExecBinary final : public ExecExpression {
public:
    ExecBinary(PlanBinaryOp op,
               ExecExprPtr left,
               ExecExprPtr right,
               DataType return_type)
        : ExecExpression(Type::BINARY, return_type),
          op_(op), left_(std::move(left)), right_(std::move(right)) {}

    ExecValue Eval(const VectorBatch &batch, uint32_t physical_row) const override;

private:
    PlanBinaryOp op_;
    ExecExprPtr left_;
    ExecExprPtr right_;
};

ExecExprPtr CompileExecExpr(const PlanExpr &expr, const ExecSchema &input_schema);

} // namespace simple_olap
