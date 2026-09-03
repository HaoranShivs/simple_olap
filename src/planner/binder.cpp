#include "binder.h"

#include <stdexcept>

namespace simple_olap
{

    // ==================== BinderContext ====================

    // 根据列名查找表 OID 和列索引
    std::optional<std::pair<uint32_t, uint32_t>> BinderContext::FindColumn(const std::string &col_name)
    {
        for (const auto &table : tables_)
        {
            for (size_t i = 0; i < table.columns.size(); ++i)
            {
                if (table.columns[i].name == col_name)
                {
                    return std::make_pair(table.oid, static_cast<uint32_t>(i));
                }
            }
        }
        return std::nullopt;
    }

    // 根据表 OID 和列索引获取列类型
    DataType BinderContext::GetColumnType(uint32_t table_oid, uint32_t col_idx) const
    {
        for (const auto &table : tables_)
        {
            if (table.oid != table_oid)
                continue;
            if (col_idx >= table.columns.size())
                break;
            return table.columns[col_idx].type;
        }
        return DataType::INVALID;
    }

    // ==================== Binder ====================

    // 1. 总入口
    std::unique_ptr<BoundSelectStatement> Binder::BindSelect(const SelectStatement &stmt)
    {
        context_ = std::make_unique<BinderContext>();

        // 第一步：绑定 FROM 子句，构建 Context
        BindTableRef(stmt.table_name);

        // 第二步：绑定 SELECT, WHERE, GROUP BY
        auto bound_select_list = BindSelectList(stmt.select_list);
        auto bound_where = stmt.where_clause ? BindExpr(*stmt.where_clause) : nullptr;
        auto bound_group_by = BindGroupBy(stmt.group_by);

        // 第三步：OLAP 核心语义校验 (检查 SELECT 列是否合法)
        ValidateAggregations(bound_select_list, bound_group_by);

        return std::make_unique<BoundSelectStatement>(
            context_->tables_[0].oid,
            std::move(bound_select_list),
            std::move(bound_where),
            std::move(bound_group_by));
    }

    // 2. 表达式绑定接口：按 AST 表达式类型分发
    std::unique_ptr<BoundExpr> Binder::BindExpr(const Expr &expr)
    {
        switch (expr.type)
        {
        case Expr::Type::COLUMN_REF:
            return BindColumnRef(static_cast<const ColumnRefExpr &>(expr));
        case Expr::Type::LITERAL:
            return BindLiteral(static_cast<const LiteralExpr &>(expr));
        case Expr::Type::BINARY_OP:
            return BindBinaryOp(static_cast<const BinaryOpExpr &>(expr));
        case Expr::Type::AGG_FUNC:
            // TODO: 聚合函数绑定
            throw SemanticException("Aggregate function binding not implemented yet.");
        }
        throw SemanticException("Unknown expression type.");
    }

    // 3. 列绑定实现 (展示名称解析过程)
    std::unique_ptr<BoundExpr> Binder::BindColumnRef(const ColumnRefExpr &expr)
    {
        auto res = context_->FindColumn(expr.column_name);
        if (!res.has_value())
        {
            throw SemanticException("Column not found: " + expr.column_name);
        }
        auto [table_oid, col_idx] = res.value();
        auto dtype = context_->GetColumnType(table_oid, col_idx);
        return std::make_unique<BoundColumnRef>(table_oid, col_idx, dtype);
    }

    // 绑定字面量：值在绑定阶段已确定，只需推导出对应的 DataType
    std::unique_ptr<BoundExpr> Binder::BindLiteral(const LiteralExpr &expr)
    {
        DataType dtype = DataType::INVALID;
        if (std::holds_alternative<int32_t>(expr.value))
            dtype = DataType::INT32;
        else if (std::holds_alternative<double>(expr.value))
            dtype = DataType::DOUBLE;
        else if (std::holds_alternative<std::string>(expr.value))
            dtype = DataType::VARCHAR;

        return std::make_unique<BoundLiteral>(expr.value, dtype);
    }

    // 绑定二元操作：递归绑定左右子表达式，并推导结果类型
    std::unique_ptr<BoundExpr> Binder::BindBinaryOp(const BinaryOpExpr &expr)
    {
        auto left = BindExpr(*expr.left);
        auto right = BindExpr(*expr.right);

        // 类型推导：任一侧为 DOUBLE 则结果为 DOUBLE，否则沿用左侧类型
        DataType dtype = (left->return_type == DataType::DOUBLE || right->return_type == DataType::DOUBLE)
                             ? DataType::DOUBLE
                             : left->return_type;

        return std::make_unique<BoundBinaryOp>(expr.op, std::move(left), std::move(right), dtype);
    }

    // 4. 聚合合法性校验 (OLAP 必须)
    void Binder::ValidateAggregations(const std::vector<std::unique_ptr<BoundExpr>> &select_list,
                                      const std::vector<std::unique_ptr<BoundExpr>> &group_by)
    {
        bool has_group_by = !group_by.empty();
        for (const auto &expr : select_list)
        {
            if (!IsAggregate(*expr) && has_group_by)
            {
                if (!IsInGroupBy(*expr, group_by))
                {
                    throw SemanticException("SELECT column must appear in GROUP BY clause.");
                }
            }
        }
    }

    // 5. 聚合判断辅助函数
    bool Binder::IsAggregate(const BoundExpr &expr)
    {
        return expr.type == BoundExpr::Type::AGG_FUNC;
    }

    bool Binder::IsInGroupBy(const BoundExpr &expr, const std::vector<std::unique_ptr<BoundExpr>> &group_by)
    {
        // 遍历 GROUP BY 列表，只要找到一个语义等价的表达式即可
        for (const auto &g_expr : group_by)
        {
            if (BoundExpr::IsExprEqual(expr, *g_expr))
            {
                return true;
            }
        }
        return false;
    }

    // 绑定 FROM 子句中的表引用：
    // 1. 通过 Catalog 按名字查找表，不存在则报语义错误
    // 2. 取出表 Schema（列名 + 列类型），连同表 OID 一起记录到 BinderContext
    void Binder::BindTableRef(const std::string &table_name)
    {
        // 1. 查表：仅查元数据，不要求表已载入内存
        auto table_oid = catalog_.FindTable(table_name);
        if (!table_oid.has_value())
        {
            throw SemanticException("Table not found: " + table_name);
        }

        // 2. 取 Table 并读取 Schema（GetTable 内部会自动 Load 未载入的表）
        Table *table = catalog_.GetTable(table_name);
        if (table == nullptr)
        {
            throw SemanticException("Failed to load table: " + table_name);
        }
        const TableSchema &schema = table->GetSchema();

        // 3. 记录到上下文，供后续列名解析使用
        BoundTable bound;
        bound.oid = table_oid.value();
        bound.alias = table_name;
        bound.columns.reserve(schema.columns.size());
        for (const auto &col : schema.columns)
        {
            bound.columns.push_back(col);
        }
        context_->tables_.push_back(std::move(bound));
    }

    std::vector<BndExprPtr> Binder::BindSelectList(std::vector<SelectItem> select_list)
    {
        std::vector<BndExprPtr> bound_list;
        bound_list.reserve(select_list.size());

        for (auto &item : select_list)
        {
            // SelectItem 持有 AST 表达式 (ExprPtr)，递归绑定成 BoundExpr
            bound_list.push_back(BindExpr(*item.expr));
        }
        return bound_list;
    }

    std::vector<BndExprPtr> Binder::BindGroupBy(std::vector<ExprPtr> group_by)
    {
        std::vector<BndExprPtr> bound_list;
        bound_list.reserve(group_by.size());

        for (auto &expr : group_by)
        {
            bound_list.push_back(BindExpr(*expr));
        }
        return bound_list;
    }

} // namespace simple_olap
