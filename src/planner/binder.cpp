#include "binder.h"

#include <stdexcept>
#include <unordered_set>

namespace simple_olap
{
    // 类型兼容性检查：INSERT 值类型与目标列类型是否匹配
    // 简化策略：完全相同才兼容；数值类型间暂不做隐式转换（后续可扩展 Cast）
    static bool IsTypeCompatible(DataType expected, DataType actual)
    {
        return expected == actual;
    }

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

    BoundStatementPtr Binder::BindCreateTable(const CreateTableStatement &stmt)
    {
        // 1. 检查表是否已存在（Catalog::FindTable 仅查元数据，未找到返回空 optional）
        if (catalog_.FindTable(stmt.table_name).has_value())
        {
            throw SemanticException("Table already exists: " + stmt.table_name);
        }

        // 2. 校验列定义
        std::unordered_set<std::string> seen_cols;
        std::vector<BoundColumnDef> bound_cols;
        bound_cols.reserve(stmt.columns.size());

        for (const auto &col : stmt.columns)
        {
            // 校验空列名
            if (col.name.empty())
            {
                throw SemanticException("Column name cannot be empty");
            }
            // 校验列名重复
            if (seen_cols.count(col.name))
            {
                throw SemanticException("Duplicate column name: " + col.name);
            }
            seen_cols.insert(col.name);

            // 校验数据类型
            if (col.type == DataType::INVALID)
            {
                throw SemanticException("Invalid data type for column: " + col.name);
            }

            bound_cols.push_back({col.name, col.type});
        }

        // 3. 组装返回
        auto result = std::make_unique<BoundCreateTableStatement>();
        result->table_name = stmt.table_name;
        result->columns = std::move(bound_cols);
        return result;
    }

    BoundStatementPtr Binder::BindInsert(const InsertStatement &stmt)
    {
        // 1. 查表，获取 Table（GetTable 不存在返回 nullptr，存在但未载入会自动 Load）
        Table *table = catalog_.GetTable(stmt.table_name);
        if (table == nullptr)
        {
            throw SemanticException("Table not found: " + stmt.table_name);
        }
        const TableSchema &schema = table->GetSchema();

        // 2. 确定目标列及其物理索引
        std::vector<uint32_t> target_indices;

        if (stmt.columns.empty())
        {
            // 情况 A: 未指定列名，默认插入所有列
            // 校验第一行的值数量是否匹配全表列数
            if (!stmt.values.empty() && stmt.values[0].size() != schema.columns.size())
            {
                throw SemanticException("INSERT VALUES size mismatch with table schema");
            }
            for (uint32_t i = 0; i < schema.columns.size(); ++i)
            {
                target_indices.push_back(i);
            }
        }
        else
        {
            // 情况 B: 指定了列名，需要建立 逻辑列名 -> 物理索引 的映射
            if (!stmt.values.empty() && stmt.values[0].size() != stmt.columns.size())
            {
                throw SemanticException("INSERT VALUES size mismatch with specified columns");
            }

            std::unordered_set<std::string> seen;
            for (const auto &col_name : stmt.columns)
            {
                if (seen.count(col_name))
                {
                    throw SemanticException("Duplicate column in INSERT: " + col_name);
                }
                seen.insert(col_name);

                // 在 Schema 中查找该列的物理索引（ColumnSchema 无 idx 字段，用遍历下标）
                bool found = false;
                for (size_t j = 0; j < schema.columns.size(); ++j)
                {
                    if (schema.columns[j].name == col_name)
                    {
                        target_indices.push_back(static_cast<uint32_t>(

j));
                        found = true;
                        break;
                    }
                }
                if (!found)
                {
                    throw SemanticException("Column not found in table: " + col_name);
                }
            }
        }

        // 3. 绑定 VALUES 并做类型检查
        std::vector<std::vector<std::unique_ptr<BoundExpr>>> bound_values;
        bound_values.reserve(stmt.values.size());

        for (const auto &row : stmt.values)
        {
            if (row.size() != target_indices.size())
            {
                throw SemanticException("INSERT row values size mismatch");
            }

            std::vector<std::unique_ptr<BoundExpr>> bound_row;
            bound_row.reserve(row.size());

            for (size_t i = 0; i < row.size(); ++i)
            {
                // 绑定表达式 (通常是字面量，也可能是简单的算术表达式)
                auto bound_expr = BindExpr(*row[i]);

                // 类型校验
                uint32_t physical_col_idx = target_indices[i];
                DataType expected_type = schema.columns[physical_col_idx].type;
                DataType actual_type = bound_expr->return_type;

                if (!IsTypeCompatible(expected_type, actual_type))
                {
                    throw SemanticException("Type mismatch in INSERT for column index " +
                                            std::to_string(physical_col_idx));
                }

                // 简化版暂不实现隐式类型转换 (Cast)
                // 若需要，可在此处插入: bound_expr = AddCastIfNeeded(std::move(bound_expr), expected_type);

                bound_row.push_back(std::move(bound_expr));
            }
            bound_values.push_back(std::move(bound_row));
        }

        // 4. 组装返回（table_oid 从 FindTable 获取，TableSchema 本身不含 oid）
        auto result = std::make_unique<BoundInsertStatement>();
        result->table_oid = catalog_.FindTable(stmt.table_name).value();
        result->target_col_indices = std::move(target_indices);
        result->values = std::move(bound_values);
        return result;
    }

    // 1. 总入口
    BoundStatementPtr Binder::BindSelect(const SelectStatement &stmt)
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
