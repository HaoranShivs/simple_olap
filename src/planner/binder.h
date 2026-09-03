#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>
#include <stdexcept>

#include "../storage/catalog.h"
#include "../sql/ast/boundexpr.h"
#include "../sql/ast/expression.h"
#include "../sql/ast/statement.h"
#include "../sql/ast/boundstat.h"

namespace simple_olap
{
    class SemanticException : public std::runtime_error
    {
    public:
        explicit SemanticException(const std::string &message)
            : std::runtime_error("Semantic Error: " + message) {}
    };

    struct BoundTable
    {
        uint32_t oid;
        std::string alias;
        std::vector<ColumnSchema> columns;
    };

    class BinderContext
    {
    public:
        // 记录当前查询涉及的表（支持后续 JOIN 的扩展）
        std::vector<BoundTable> tables_;

        // 根据列名查找表 OID 和列索引
        std::optional<std::pair<uint32_t, uint32_t>> FindColumn(const std::string &col_name);

        // 根据表 OID 和列索引获取列类型
        DataType GetColumnType(uint32_t table_oid, uint32_t col_idx) const;
    };

    class Binder
    {
    public:
        explicit Binder(Catalog &catalog) : catalog_(catalog) {}

        // 1. 总入口
        BoundStatementPtr BindStatement(const Statement& stmt) {
        switch (stmt.GetType()) {
            case Statement::Type::SELECT:
                return BindSelect(static_cast<const SelectStatement&>(stmt));
            
            case Statement::Type::INSERT:
                return BindInsert(static_cast<const InsertStatement&>(stmt));
            
            case Statement::Type::CREATE_TABLE:
                return BindCreateTable(static_cast<const CreateTableStatement&>(stmt));
            
            default:
                throw SemanticException("Unsupported statement type");
        }
    }

    private:
        std::unique_ptr<BoundSelectStatement> BindSelect(const SelectStatement &stmt);

        std::unique_ptr<BoundInsertStatement> BindInsert(const SelectStatement &stmt);

        std::unique_ptr<BoundCreateTableStatement> BindCreateTable(const SelectStatement &stmt);

        // 2. 表达式绑定接口
        std::unique_ptr<BoundExpr> BindExpr(const Expr &expr);

        // 3. 列绑定实现 (展示名称解析过程)
        std::unique_ptr<BoundExpr> BindColumnRef(const ColumnRefExpr &expr);

        std::unique_ptr<BoundExpr> BindLiteral(const LiteralExpr &expr);

        std::unique_ptr<BoundExpr> BindBinaryOp(const BinaryOpExpr &expr);

        // 4. 聚合合法性校验 (OLAP 必须)
        void ValidateAggregations(const std::vector<std::unique_ptr<BoundExpr>> &select_list,
                                  const std::vector<std::unique_ptr<BoundExpr>> &group_by);

        // 5. 聚合判断辅助函数
        bool IsAggregate(const BoundExpr &expr);

        bool IsInGroupBy(const BoundExpr &expr, const std::vector<std::unique_ptr<BoundExpr>> &group_by);

        void BindTableRef(const std::string &table_name);

        std::vector<BndExprPtr> BindSelectList(std::vector<SelectItem> select_list);

        std::vector<BndExprPtr> BindGroupBy(std::vector<ExprPtr> group_by);

        Catalog &catalog_;
        std::unique_ptr<BinderContext> context_;
    };
} // namespace simple_olap
