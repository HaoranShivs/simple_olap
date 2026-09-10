#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "../catalog/catalog.h"
#include "../sql/ast/boundexpr.h"
#include "../sql/ast/boundstat.h"
#include "../sql/ast/expression.h"
#include "../sql/ast/statement.h"

namespace simple_olap {
class SemanticException : public std::runtime_error {
  public:
    explicit SemanticException(const std::string& message) : std::runtime_error("Semantic Error: " + message) {}
};

struct BoundTable {
    uint32_t oid;
    std::string alias;
    std::vector<ColumnSchema> columns;
};

class BinderContext {
  public:
    // 记录当前查询涉及的表（支持后续 JOIN 的扩展）
    std::vector<BoundTable> tables_;

    // 根据列名查找表 OID 和列索引
    std::optional<std::pair<uint32_t, uint32_t>> FindColumn(const std::string& col_name);

    // 根据表 OID 和列索引获取列类型
    DataType GetColumnType(uint32_t table_oid, uint32_t col_idx) const;
};

// 语义绑定器：把 AST 解析为带类型与列来源信息的 BoundStatement。
// 负责名称解析、类型推导，以及聚合查询的合法性校验。
class Binder {
  public:
    explicit Binder(Catalog& catalog) : catalog_(catalog) {}

    // 语句绑定总入口：按语句类型分发。
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
    // ---------- 语句绑定 ----------
    BoundStatementPtr BindSelect(const SelectStatement& stmt);

    BoundStatementPtr BindInsert(const InsertStatement& stmt);

    BoundStatementPtr BindCreateTable(const CreateTableStatement& stmt);

    // ---------- 表达式绑定 ----------
    std::unique_ptr<BoundExpr> BindExpr(const Expr& expr);

    std::unique_ptr<BoundExpr> BindColumnRef(const ColumnRefExpr& expr);

    std::unique_ptr<BoundExpr> BindLiteral(const LiteralExpr& expr);

    std::unique_ptr<BoundExpr> BindBinaryOp(const BinaryOpExpr& expr);

    std::unique_ptr<BoundExpr> BindAggFunc(const AggFuncExpr& expr);

    // ---------- 语义校验与辅助 ----------
    // 校验 SELECT 项与 GROUP BY 的聚合合法性。
    void ValidateAggregations(const std::vector<BoundSelectItem>& select_list,
                              const std::vector<std::unique_ptr<BoundExpr>>& group_by);

    bool IsInGroupBy(const BoundExpr& expr, const std::vector<std::unique_ptr<BoundExpr>>& group_by);

    void BindTableRef(const std::string& table_name);

    std::vector<BoundSelectItem> BindSelectList(const std::vector<SelectItem>& select_list);

    std::vector<BndExprPtr> BindGroupBy(const std::vector<ExprPtr>& group_by);

    bool ContainsAggregate(const BoundExpr& expr) const;

    Catalog& catalog_;
    std::unique_ptr<BinderContext> context_;
};
} // namespace simple_olap
