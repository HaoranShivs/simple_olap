#include "scan.h"

#include <stdexcept>

#include "../../sql/ast/expression.h"

namespace simple_olap
{
    namespace
    {
        // BinaryOpExpr 的比较类操作符 -> 存储层 CmpOp
        CmpOp ToCmpOp(BinaryOpExpr::OpType op)
        {
            switch (op)
            {
            case BinaryOpExpr::OpType::EQ:
                return CmpOp::EQ;
            case BinaryOpExpr::OpType::GT:
                return CmpOp::GT;
            case BinaryOpExpr::OpType::LT:
                return CmpOp::LT;
            default:
                throw std::runtime_error(
                    "TableScan - unsupported binary operator in WHERE");
            }
        }
    } // namespace

    TableScan::TableScan(const SelectStatement &selectstat, Catalog *catalog)
        : 
        selectstat_(&selectstat), catalog_(catalog), table_(nullptr)
    {
    }

    // 查看表是否存在，并绑定表。
    void TableScan::Init()
    {
        // 1. 按表名在 catalog 中查找表（存在但未载入内存时 GetTable 会自动 LoadTable）
        table_ = catalog_->GetTable(selectstat_->table_name);
        if (table_ == nullptr)
        {
            throw std::runtime_error("TableScan::Init - table not found: " +
                                     selectstat_->table_name);
        }

        // 2. 一次性绑定 request_：把投影表达式解析为 column_id（按 select_list 顺序输出），
        //    Next() 直接复用，避免每批数据都重复转换。
        //    目前仅支持简单列引用（ColumnRefExpr）；聚合/表达式投影待执行层升级后接入。
        const TableSchema &schema = table_->GetSchema();
        request_ = SelectTargetStatement{};
        request_.target_list.reserve(selectstat_->select_list.size());
        for (const auto &item : selectstat_->select_list)
        {
            if (item.expr->type != Expr::Type::COLUMN_REF)
            {
                throw std::runtime_error(
                    "TableScan::Init - only simple column projection is supported");
            }
            const auto &col = static_cast<const ColumnRefExpr &>(*item.expr);
            const ColumnId 
            col_id = FindColumnId(schema, 
            col.column_name);
            request_.target_list.push_back(SelectTarget{col_id, AggType::INVALID});
        }

        // 3. 绑定 where 条件：目前仅支持 column cmp literal 形式（BinaryOpExpr），
        //    列名 -> column_id，透传比较符与常量值。
        //    where 列若不在输出列中无需在此追加——
        //    StorageManager::GetVectorBatch 会在行级过滤前自行补齐并移除。
        if (selectstat_->where_clause != nullptr)
        {
            const auto &bin = static_cast<const BinaryOpExpr &>(*selectstat_->where_clause);
            if (bin.left->type != Expr::Type::COLUMN_REF ||
                bin.right->type != Expr::Type::LITERAL)
            {
                throw std::runtime_error(
                    "TableScan::Init - only 'column cmp literal' WHERE is supported");
            }
            const auto &col = static_cast<const ColumnRefExpr &>(*bin.left);
            const auto &lit = static_cast<const LiteralExpr &>(*bin.right);

            auto expr = std::make_unique<ExprTarget>();
            expr->column_id = FindColumnId(schema, col.column_name);
            expr->op = ToCmpOp(bin.op);
            expr->value = std::get<int32_t>(lit.value);
            request_.where = std::move(expr);
        }

        // 4. 重置扫描游标（从头开始）
        cursor_ = ScanCursor{};
    }

    // 拉取下一批数据：委托 Table -> StorageManager -> SegmentReader，
    // 由 cursor_ 记录跨 segment 的扫描位置，每次产出一批（最多 1024 行）。
    // 返回 false 表示所有 segment 已读完（数据流结束）。
    bool TableScan::Next(VectorBatch &batch)
    {
        if (table_ == nullptr)
        {
            throw std::runtime_error("TableScan::Next - Init() not called");
        }

        // 每批开始前清空上一批的残留状态（列结构由下层重建）
        batch.Reset();

        return table_->GetVectorBatch(request_, cursor_, batch);
    }

    // 私有辅助：按列名在 schema 中查找 column_id；未找到抛出异常
    ColumnId TableScan::FindColumnId(const TableSchema &schema,
                                     const std::string &column_name)
    {
        for (const auto &col : schema.columns)
        {
            if (col.name == column_name)
            {
                return col.column_id;
            }
        }
        throw std::runtime_error("TableScan - column not found: " + column_name);
    }

} // namespace simple_olap
