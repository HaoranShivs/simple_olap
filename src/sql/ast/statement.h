#pragma once

#include <memory>
#include <string>
#include <vector>

#include "expression.h"

namespace simple_olap
{

    // 前向声明
    class StatementVisitor; // 用于执行器遍历

    /// @brief SELECT 子句中的单个投影项
    struct SelectItem
    {
        ExprPtr expr;      // 可以是 ColumnRef, AggFunc, 甚至是 BinaryOp (a+b)
        std::string alias; // 别名，为空则使用默认名

        explicit SelectItem(ExprPtr expr, std::string alias = "")
            : expr(std::move(expr)), alias(std::move(alias)) {}
    };

    /// @brief 语句基类
    struct Statement
    {
        // 1. 语句类型枚举
        // 为什么用 enum 而不是 dynamic_cast？
        // 数据库引擎对性能极其敏感，枚举配合 switch-case 比 RTTI (dynamic_cast) 快得多。
        enum class Type
        {
            SELECT,
            INSERT,
            UPDATE,
            DELETE,
            CREATE_TABLE,
            EXPLAIN // OLAP 和调试必备
        };

        explicit Statement(Type type) : type_(type) {}
        virtual ~Statement() = default; // 多态基类必须有虚析构函数，防止内存泄漏

        /// @brief 获取类型
        Type GetType() const { return type_; }

        // 2. 调试接口 (极其重要！)
        // 将 AST 转换回可读的字符串，用于打印日志、报错和 EXPLAIN 功能。
        virtual std::string ToString() const = 0;

        // 3. 访问者模式接口 (为后续的 Executor 和 Optimizer 预留)
        virtual void Accept(StatementVisitor *visitor) const = 0;

    protected:
        Type type_;
    };

    using StatementPtr = std::unique_ptr<Statement>;

    /// @brief SELECT 语句
    struct SelectStatement : public Statement
    {
        std::string table_name;
        std::vector<SelectItem> select_list;

        ExprPtr where_clause;          // 现在可以完美支持 WHERE a > 1 AND b = 2
        std::vector<ExprPtr> group_by; // 通常是 ColumnRefExpr，但用 ExprPtr 更通用

        // 后续可以轻松添加：
        // ExprPtr having_clause;
        // std::vector<OrderByItem> order_by;

        SelectStatement() : Statement(Type::SELECT) {}

        std::string ToString() const override;
        void Accept(StatementVisitor *visitor) const override;
    };

    class InsertStatement : public Statement
    {
    public:
        InsertStatement() : Statement(Type::INSERT) {}

        std::string table_name;

        // 显式指定的列名，例如：INSERT INTO t (a, b) VALUES ...
        // 如果为空，表示插入所有列：INSERT INTO t VALUES ...
        std::vector<std::string> columns;

        // 插入的值列表：外层 vector 代表多行，内层 vector 代表单行的多个值
        // 使用 ExprPtr 可以支持字面量、表达式甚至函数调用
        std::vector<std::vector<ExprPtr>> values;

        std::string ToString() const override
        {
            std::string sql = "INSERT INTO " + table_name;

            if (!columns.empty())
            {
                sql += " (";
                for (size_t i = 0; i < columns.size(); ++i)
                {
                    sql += columns[i];
                    if (i < columns.size() - 1)
                        sql += ", ";
                }
                sql += ")";
            }

            sql += " VALUES ";
            for (size_t i = 0; i < values.size(); ++i)
            {
                sql += "(";
                for (size_t j = 0; j < values[i].size(); ++j)
                {
                    sql += values[i][j]->ToString();
                    if (j < values[i].size() - 1)
                        sql += ", ";
                }
                sql += ")";
                if (i < values.size() - 1)
                    sql += ", ";
            }
            return sql;
        }

        void Accept(StatementVisitor *visitor) const override
        {
            // visitor->Visit(*this);
        }
    };

    class CreateTableStatement : public Statement
    {
    public:
        CreateTableStatement() : Statement(Type::CREATE_TABLE) {}

        // 1. 基础信息
        std::string table_name;
        bool if_not_exists = false; // 支持 CREATE TABLE IF NOT EXISTS

        // 2. 列定义列表 (使用值类型 vector，内存连续，访问极快)
        std::vector<ColumnSchema> columns;

        // 暂时不考虑
        // // 3. OLAP 核心特性：排序键 (Sort Key / Order By Key)
        // // 在 OLAP 引擎(如 LSM-Tree/MergeTree)中，数据会按此顺序物理排序。
        // // 这能极大加速带有这些列前缀的 WHERE 过滤和 GROUP BY 查询。
        // std::vector<std::string> sort_keys;

        // (进阶预留) 分区键 (Partition Key)
        // std::vector<std::string> partition_keys;

        // 4. 实现基类的纯虚函数
        std::string ToString() const override
        {
            std::string sql = "CREATE TABLE ";
            if (if_not_exists)
                sql += "IF NOT EXISTS ";
            sql += table_name + " (\n";

            for (size_t i = 0; i < columns.size(); ++i)
            {
                sql += "  " + columns[i].ToString();
                if (i < columns.size() - 1)
                    sql += ",\n";
            }
            sql += "\n)";

            // 打印 OLAP 特性
            if (!sort_keys.empty())
            {
                sql += " ORDER BY (";
                for (size_t i = 0; i < sort_keys.size(); ++i)
                {
                    sql += sort_keys[i];
                    if (i < sort_keys.size() - 1)
                        sql += ", ";
                }
                sql += ")";
            }

            return sql;
        }

        void Accept(StatementVisitor *visitor) const override
        {
            // 后续实现 Visitor 模式时补充
            // visitor->Visit(*this);
        }
    };

} // namespace simple_olap
