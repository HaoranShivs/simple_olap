#include "boundexpr.h"

namespace simple_olap
{
    bool BoundExpr::IsExprEqual(const BoundExpr &a, const BoundExpr &b)
    {
        // 1. 类型不同，必定不等价
        if (a.type != b.type)
        {
            return false;
        }

        // 2. 根据类型进行深度比较
        switch (a.type)
        {
        case BoundExpr::Type::COLUMN_REF:
        {
            const auto &col_a = static_cast<const BoundColumnRef &>(a);
            const auto &col_b = static_cast<const BoundColumnRef &>(b);
            // 物理 OID 和 索引相同，即为同一列
            return col_a.table_oid == col_b.table_oid && col_a.col_idx == col_b.col_idx;
        }
        case BoundExpr::Type::LITERAL:
        {
            const auto &lit_a = static_cast<const BoundLiteral &>(a);
            const auto &lit_b = static_cast<const BoundLiteral &>(b);
            // 比较 variant 中的实际值 (假设你用了 std::variant)
            return lit_a.value == lit_b.value;
        }
        case BoundExpr::Type::BINARY_OP:
        {
            const auto &op_a = static_cast<const BoundBinaryOp &>(a);
            const auto &op_b = static_cast<const BoundBinaryOp &>(b);
            // 操作符相同，且左右子树递归等价
            return op_a.op == op_b.op &&
                   IsExprEqual(*op_a.left, *op_b.left) &&
                   IsExprEqual(*op_a.right, *op_b.right);
        }
        case BoundExpr::Type::AGG_FUNC:
        {
            const auto &agg_a = static_cast<const BoundAggFunc &>(a);
            const auto &agg_b = static_cast<const BoundAggFunc &>(b);
            // 聚合类型相同，且内部参数等价 (例如 SUM(a) 和 SUM(a))
            return agg_a.agg_type == agg_b.agg_type &&
                   IsExprEqual(*agg_a.arg, *agg_b.arg);
        }
        // ... 其他类型 (CAST, FUNCTION 等)
        default:
            return false;
        }
    }
} // namespace simple_olap
