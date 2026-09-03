#pragma once

#include <string>
#include <vector>
#include <cstdint>

#include "boundexpr.h"

namespace simple_olap
{

    // 前向声明
    class BoundSelectStatement;
    class BoundInsertStatement;
    class BoundCreateTableStatement;
    class BoundStatementVisitor;

    // ==========================================
    // 1. Bound Statement 基类
    // ==========================================
    class BoundStatement
    {
    public:
        enum class Type
        {
            SELECT,
            INSERT,
            CREATE_TABLE,
            EXPLAIN,
            // 未来扩展...
        };

        explicit BoundStatement(Type type) : type_(type) {}
        virtual ~BoundStatement() = default;

        Type GetType() const { return type_; }

        // 调试接口：将绑定后的计划打印出来
        virtual std::string ToString() const = 0;

        // 为执行器预留的访问者接口
        virtual void Accept(BoundStatementVisitor *visitor) const = 0;

    protected:
        Type type_;
    };

    using BoundStatementPtr = std::unique_ptr<BoundStatement>;

    /// @brief SELECT 子句中的单个投影项 (例如: age, COUNT(*) AS total)
    struct BoundSelectItem
    {
        // 必须使用 unique_ptr 存储多态基类，防止对象切片！
        BndExprPtr expr;

        std::string alias; // 别名，为空则 Executor 会尝试生成默认名

        // 构造函数：接收 unique_ptr 的所有权
        BoundSelectItem(BndExprPtr e, std::string a = "")
            : expr(std::move(e)), alias(std::move(a)) {}

        // 禁用拷贝构造，因为 unique_ptr 是 move-only 的
        BoundSelectItem(const BoundSelectItem &) = delete;
        BoundSelectItem &operator=(const BoundSelectItem &) = delete;

        // 允许移动构造
        BoundSelectItem(BoundSelectItem &&) = default;
        BoundSelectItem &operator=(BoundSelectItem &&) = default;
    };

    /// @brief 绑定后的完整 SELECT 语句
    class BoundSelectStatement : public BoundStatement
    {
    public:
        uint32_t table_oid; // 绑定的物理表 ID (暂不考虑 JOIN)

        // 核心：包含多个投影项
        std::vector<BoundSelectItem> select_list;

        BndExprPtr where_clause;
        std::vector<BndExprPtr> group_by;

        BoundSelectStatement(uint32_t t_oid,
                             std::vector<BoundSelectItem> s_list,
                             BndExprPtr where,
                             std::vector<BndExprPtr> g_by)
            : BoundStatement(Type::SELECT),
              table_oid(t_oid),
              select_list(std::move(s_list)),
              where_clause(std::move(where)),
              group_by(std::move(g_by)) {}

        std::string ToString() const override
        {
            return "BoundSelectStatement(...)"; // 具体实现省略
        }

        void Accept(BoundStatementVisitor *visitor) const override;
    };

    // ==========================================
    // 3. 其他命令的 Bound 实现 (示例)
    // ==========================================
    class BoundInsertStatement : public BoundStatement
    {
    public:
        BoundInsertStatement() : BoundStatement(Type::INSERT) {}
        uint32_t table_oid;
        // 目标列的物理索引（INSERT 未指定列名时为全列顺序 0..n-1）
        std::vector<uint32_t> target_col_indices;
        std::vector<std::vector<std::unique_ptr<BoundExpr>>> values;

        std::string ToString() const override { return "BoundInsertStatement(...)"; }
        void Accept(BoundStatementVisitor *visitor) const override;
    };

    /// @brief 绑定后的列定义 (CREATE TABLE 用)
    struct BoundColumnDef
    {
        std::string name;
        DataType type;
    };

    /// @brief 绑定后的 CREATE TABLE 语句
    class BoundCreateTableStatement : public BoundStatement
    {
    public:
        BoundCreateTableStatement() : BoundStatement(Type::CREATE_TABLE) {}
        std::string table_name;
        std::vector<BoundColumnDef> columns;

        std::string ToString() const override { 
            return "BoundCreateTableStatement(...)"; }
        void Accept(BoundStatementVisitor *visitor) const override;
    };
} // namespace simple_olap
