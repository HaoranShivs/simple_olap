#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <variant>

namespace simple_olap
{

    // 前向声明
    struct Expr;
    using ExprPtr = std::unique_ptr<Expr>;

    /// @brief 比较运算符
    enum class CmpOp : uint8_t
    {
        EQ,
        NE,
        GT,
        GE,
        LT,
        LE
    };

    /// @brief 聚合函数类型
    enum class AggType : uint8_t
    {
        INVALID = 0,
        SUM,   // 求和
        COUNT, // 计数
        AVG,   // 平均值（内部用 SUM + COUNT 实现）
        MIN,   // 最小值
        MAX    // 最大值
    };

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
    };

    /// @brief 列引用 (如: SELECT a 中的 'a', GROUP BY a 中的 'a')
    struct ColumnRefExpr : public Expr
    {
        std::string column_name;

        explicit ColumnRefExpr(std::string name)
            : Expr(Type::COLUMN_REF), column_name(std::move(name)) {}
    };

    /// @brief 字面量 (如: WHERE a > 18 中的 '18')
    struct LiteralExpr : public Expr
    {
        std::variant<int32_t, std::string, double> value; // 支持多种数据类型

        explicit LiteralExpr(int32_t v) : Expr(Type::LITERAL), value(v) {}
        explicit LiteralExpr(double v) : Expr(Type::LITERAL), value(v) {}
        explicit LiteralExpr(std::string v) : Expr(Type::LITERAL), value(std::move(v)) {}
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
    };

    /// @brief 聚合函数
    struct AggFuncExpr : public Expr
    {
        AggType agg_type;
        ExprPtr arg; // 聚合的参数，例如 SUM(arg)

        AggFuncExpr(AggType agg_type, ExprPtr arg)
            : Expr(Type::AGG_FUNC), agg_type(agg_type), arg(std::move(arg)) {}
    };

} // namespace simple_olap
