#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <variant>

#include "../../planner/plan_expression.h"
#include "../schema.h"
#include "../vector/vector.h"

namespace simple_olap {

// 执行期标量值：表达式求值结果统一用它表示。
using ExecValue = std::variant<int32_t, int64_t, float, double, std::string, bool>;

// ---------- 列读写与标量运算 ----------

// 按列类型从 physical_row 位置读出一个 ExecValue（VARCHAR 走定长槽）。
ExecValue ReadExecValue(const ColumnData& column, uint32_t physical_row);

// 按列类型把 value 写到 physical_row 位置；类型不匹配会抛异常。
void WriteExecValue(ColumnData& column, uint32_t physical_row, const ExecValue& value);

// 真值判定：bool 直接取用，字符串非空视为真，其余按数值是否非零。
bool ExecValueAsBool(const ExecValue& value);

// 转成 long double 参与运算；字符串会抛异常。
long double ExecValueAsNumber(const ExecValue& value);

// 把数值按目标类型收窄/扩展成 ExecValue。
ExecValue CastNumber(long double value, DataType type);

// 比较两个 ExecValue：字符串按字典序，其余按数值；返回 -1 / 0 / 1。
int CompareExecValues(const ExecValue& lhs, const ExecValue& rhs);

// ---------- 表达式节点 ----------

// 执行期表达式基类：只需实现「按行求值」。
class ExecExpression {
  public:
    enum class Type : uint8_t {
        COLUMN_REF,
        LITERAL,
        BINARY,
    };

    ExecExpression(Type type, DataType return_type) : type_(type), return_type_(return_type) {}
    virtual ~ExecExpression() = default;

    Type GetType() const {
        return type_;
    }
    DataType GetReturnType() const {
        return return_type_;
    }

    virtual ExecValue Eval(const VectorBatch& batch, uint32_t physical_row) const = 0;

  private:
    Type type_;
    DataType return_type_;
};

using ExecExprPtr = std::unique_ptr<ExecExpression>;

// 列引用：直接取输入 batch 中第 input_slot 列的值。
class ExecColumnRef final : public ExecExpression {
  public:
    ExecColumnRef(uint32_t input_slot, DataType type)
        : ExecExpression(Type::COLUMN_REF, type), input_slot_(input_slot) {}

    uint32_t GetInputSlot() const {
        return input_slot_;
    }
    ExecValue Eval(const VectorBatch& batch, uint32_t physical_row) const override;

  private:
    uint32_t input_slot_;
};

// 常量：任何行求值都返回同一个值。
class ExecLiteral final : public ExecExpression {
  public:
    ExecLiteral(ExecValue value, DataType type) : ExecExpression(Type::LITERAL, type), value_(std::move(value)) {}

    ExecValue Eval(const VectorBatch&, uint32_t) const override {
        return value_;
    }

  private:
    ExecValue value_;
};

// 二元表达式：算术 / 比较 / 逻辑运算，逻辑运算按短路求值。
class ExecBinary final : public ExecExpression {
  public:
    ExecBinary(PlanBinaryOp op, ExecExprPtr left, ExecExprPtr right, DataType return_type)
        : ExecExpression(Type::BINARY, return_type), op_(op), left_(std::move(left)), right_(std::move(right)) {}

    ExecValue Eval(const VectorBatch& batch, uint32_t physical_row) const override;

  private:
    PlanBinaryOp op_;
    ExecExprPtr left_;
    ExecExprPtr right_;
};

// 把 plan 表达式编译成执行期表达式；input_schema 用于把列引用解析成输入下标。
ExecExprPtr CompileExecExpr(const PlanExpr& expr, const ExecSchema& input_schema);

} // namespace simple_olap
