#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <variant>

#include "../../type.h" // AgType 定义于公共层 type.h

namespace
simple_olap
{

    // 前向声明
    struct Expr;
    using ExprPtr = std::unique_ptr<Expr>;

    // AggType 已迁移至 src/type.h（公共层），此处不再重复定义

    /// @brief 表达式基类：所有表达式的抽象
    struct Expr
    {
        enum class Type
        {
            COLUMN_REF,
            LITERAL,
            BINARY_OP,
            AGG_FUNC
        };

        Type type;

        explicit Expr(Type type) : type(type) {}
        virtual ~Expr() = default; // 多态基类必须有虚析构函数

        /// @brief 调试接口：将表达式还原为可读字符串
        virtual std::string ToString() const = 0;
    };

    /// @brief 列引用 (如: SELECT a 中的 'a', GROUP BY a 中的 'a')
    struct ColumnRefExpr : public Expr
    {
        std::string column_name;

        explicit ColumnRefExpr(std::string name)
            : Expr(Type::COLUMN_REF), column_name(std::move(name)) {}

        std::string ToString() const override { return column_name; }
    };

    /// @brief 字面量 (如: WHERE a > 18 中的 '18')
    struct LiteralExpr : public Expr
    {
        std::variant<int32_t, std::string, double> value; // 支持多种数据类型

        explicit LiteralExpr(int32_t v) : Expr(Type::LITERAL), value(v) {}
        explicit LiteralExpr(double v) : Expr(Type::LITERAL), value(v) {}
        explicit LiteralExpr(std::string v) : Expr(Type::LITERAL), value(std::move(v)) {}

        std::string ToString() const override
        {
            if (std::holds_alternative<int32_t>(value))
                return std::to_string(std::get<int32_t>(value));
            if (std::holds_alternative<double>(value))
                return std::to_string(std::get<double>(value));
            return "'" + std::get<std::string>(value) + "'";
        }
    };

    /// @brief 二元操作 (支持算术、比较、逻辑 AND/OR)
    struct BinaryOpExpr : public Expr
    {
        enum class OpType
        {
            ADD,
            SUB,
            EQ,
            GT,
            LT,
            AND,
            OR /* ... */
        };

        OpType op;
        ExprPtr left;
        ExprPtr right;

        BinaryOpExpr(OpType op, ExprPtr left, ExprPtr right)
            : Expr(Type::BINARY_OP), op(op), left(std::move(left)), right(std::move(right)) {}

        std::string ToString() const override
        {
            static const char *op_names[] = {"+", "-", "=", ">", "<", "AND", "OR"};
            return "(" + left->ToString() + " " + op_names[static_cast<int>(op)] +
                   " " + right->ToString() + ")";
        }
    };

    /// @brief 聚合函数
    struct AggFuncExpr : public Expr
    {
        AggType agg_type;
        ExprPtr arg; // 聚合的参数，例如 SUM(arg)

        AggFuncExpr(AggType agg_type, ExprPtr arg)
            : Expr(Type::AGG_FUNC), agg_type(agg_type), arg(std::move(arg)) {}

        std::string ToString() const override
        {
            static const char *agg_names[] = {"INVALID", "SUM", "COUNT", "AVG", "MIN", "MAX"};
            std::string inner = arg ? arg->ToString() : "*";
            return std::string(agg_names[static_cast<int>(agg_type)]) + "(" + inner + ")";
        }
    };

} // namespace simple_olap
