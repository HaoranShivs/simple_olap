#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <variant>
#include <vector>

#include "../sql/ast/boundexpr.h"
#include "../type.h"

// 计划表达式：把 bound AST 表达式转成与执行方式无关的表达式树。
namespace simple_olap {

// 计划期字面量取值。
using PlanLiteralValue = std::variant<int32_t, std::string, double>;

// 计划期二元运算符，覆盖算术、比较与逻辑运算。
enum class PlanBinaryOp : uint8_t {
    ADD,
    SUB,
    EQ,
    GT,
    LT,
    AND,
    OR,
};

// 计划表达式基类：提供克隆、打印与列引用收集能力。
class PlanExpr {
  public:
    enum class Type : uint8_t {
        COLUMN_REF,
        LITERAL,
        BINARY_OP,
        AGG_FUNC,
    };

    PlanExpr(Type type, DataType return_type) : type_(type), return_type_(return_type) {}
    virtual ~PlanExpr() = default;

    Type GetType() const {
        return type_;
    }
    DataType GetReturnType() const {
        return return_type_;
    }

    virtual std::unique_ptr<PlanExpr> Clone() const = 0;
    virtual std::string ToString() const = 0;
    virtual void CollectColumnRefs(std::vector<uint32_t>& out) const = 0;
    virtual bool ContainsAggregate() const {
        return false;
    }

  private:
    Type type_;
    DataType return_type_;
};

using PlanExprPtr = std::unique_ptr<PlanExpr>;

// 列引用：记录所属表的 oid 与列下标。
class PlanColumnRef final : public PlanExpr {
  public:
    PlanColumnRef(uint32_t table_oid, uint32_t col_idx, DataType type)
        : PlanExpr(Type::COLUMN_REF, type), table_oid_(table_oid), col_idx_(col_idx) {}

    uint32_t GetTableOid() const {
        return table_oid_;
    }
    uint32_t GetColumnIndex() const {
        return col_idx_;
    }

    PlanExprPtr Clone() const override;
    std::string ToString() const override;
    void CollectColumnRefs(std::vector<uint32_t>& out) const override;

  private:
    uint32_t table_oid_;
    uint32_t col_idx_;
};

// 字面量。
class PlanLiteral final : public PlanExpr {
  public:
    PlanLiteral(PlanLiteralValue value, DataType type) : PlanExpr(Type::LITERAL, type), value_(std::move(value)) {}

    const PlanLiteralValue& GetValue() const {
        return value_;
    }

    PlanExprPtr Clone() const override;
    std::string ToString() const override;
    void CollectColumnRefs(std::vector<uint32_t>&) const override {}

  private:
    PlanLiteralValue value_;
};

// 二元表达式。
class PlanBinaryExpr final : public PlanExpr {
  public:
    PlanBinaryExpr(PlanBinaryOp op, PlanExprPtr left, PlanExprPtr right, DataType type)
        : PlanExpr(Type::BINARY_OP, type), op_(op), left_(std::move(left)), right_(std::move(right)) {}

    PlanBinaryOp GetOp() const {
        return op_;
    }
    const PlanExpr& GetLeft() const {
        return *left_;
    }
    const PlanExpr& GetRight() const {
        return *right_;
    }

    PlanExprPtr Clone() const override;
    std::string ToString() const override;
    void CollectColumnRefs(std::vector<uint32_t>& out) const override;
    bool ContainsAggregate() const override;

  private:
    PlanBinaryOp op_;
    PlanExprPtr left_;
    PlanExprPtr right_;
};

// 聚合函数表达式：arg 可以为空（COUNT(*)）。
class PlanAggExpr final : public PlanExpr {
  public:
    PlanAggExpr(AggType agg_type, PlanExprPtr arg, DataType type)
        : PlanExpr(Type::AGG_FUNC, type), agg_type_(agg_type), arg_(std::move(arg)) {}

    AggType GetAggType() const {
        return agg_type_;
    }
    const PlanExpr* GetArg() const {
        return arg_.get();
    }

    PlanExprPtr Clone() const override;
    std::string ToString() const override;
    void CollectColumnRefs(std::vector<uint32_t>& out) const override;
    bool ContainsAggregate() const override {
        return true;
    }

  private:
    AggType agg_type_;
    PlanExprPtr arg_;
};

// 带输出别名的计划表达式，用于 SELECT 列表与聚合输出。
struct NamedPlanExpr {
    PlanExprPtr expr;
    std::string alias;

    NamedPlanExpr(PlanExprPtr expr, std::string alias = {}) : expr(std::move(expr)), alias(std::move(alias)) {}

    NamedPlanExpr(NamedPlanExpr&&) noexcept = default;
    NamedPlanExpr& operator=(NamedPlanExpr&&) noexcept = default;
    NamedPlanExpr(const NamedPlanExpr&) = delete;
    NamedPlanExpr& operator=(const NamedPlanExpr&) = delete;
};

// 把 bound AST 表达式编译成计划表达式。
PlanExprPtr BuildPlanExpr(const BoundExpr& expr);

// bound 二元运算符 -> 计划二元运算符。
PlanBinaryOp ToPlanBinaryOp(BinaryOpExpr::OpType op);

// 计划二元比较运算符 -> 存储层比较运算符。
CmpOp ToCmpOp(PlanBinaryOp op);

} // namespace simple_olap
