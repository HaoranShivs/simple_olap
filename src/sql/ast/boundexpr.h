#pragma once

#include <memory>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include "../../type.h"
#include "expression.h"

namespace simple_olap {
// DataType 统一定义在 type.h（公共层），此处不再重复定义

// 绑定后表达式基类：携带类型信息与列来源。
class BoundExpr {
  public:
    enum class Type {
        COLUMN_REF,
        LITERAL,
        BINARY_OP,
        AGG_FUNC,
        CAST // 预留，暂未实现
    };
    Type type;
    DataType return_type; // 绑定阶段推导出的返回类型

    BoundExpr(Type t, DataType dtype) : type(t), return_type(dtype) {}
    virtual ~BoundExpr() = default;

    // 判断两个绑定表达式在语义上是否等价（用于 GROUP BY 匹配）。
    static bool IsExprEqual(const BoundExpr& a, const BoundExpr& b);
};

using BndExprPtr = std::unique_ptr<BoundExpr>;

// 绑定后的列引用：列名已解析为 (表 OID, 列索引)。
class BoundColumnRef : public BoundExpr {
  public:
    uint32_t table_oid;
    uint32_t col_idx;  // 物理列索引，Executor 直接用它去取数据
    std::string alias; // 仅用于最后的结果集输出展示

    BoundColumnRef(uint32_t t_oid, uint32_t c_idx, DataType dtype)
        : BoundExpr{Type::COLUMN_REF, dtype}, table_oid(t_oid), col_idx(c_idx) {}
};

// 绑定后的字面量 (值在绑定阶段已确定，Executor 直接读取)
class BoundLiteral : public BoundExpr {
  public:
    std::variant<int32_t, std::string, double> value;

    BoundLiteral(std::variant<int32_t, std::string, double> v, DataType dtype)
        : BoundExpr{Type::LITERAL, dtype}, value(std::move(v)) {}
};

// 绑定后的二元操作 (如 a + b, a > 1)
class BoundBinaryOp : public BoundExpr {
  public:
    BinaryOpExpr::OpType op;          // 运算符类型
    std::unique_ptr<BoundExpr> left;  // 左操作数
    std::unique_ptr<BoundExpr> right; // 右操作数

    BoundBinaryOp(BinaryOpExpr::OpType o, std::unique_ptr<BoundExpr> l, std::unique_ptr<BoundExpr> r, DataType dtype)
        : BoundExpr{Type::BINARY_OP, dtype}, op(o), left(std::move(l)), right(std::move(r)) {}
};

// 绑定后的聚合函数。
class BoundAggFunc : public BoundExpr {
  public:
    AggType agg_type;
    std::unique_ptr<BoundExpr> arg; // 聚合参数；COUNT(*) 时为空

    BoundAggFunc(AggType t, std::unique_ptr<BoundExpr> a, DataType dtype)
        : BoundExpr{Type::AGG_FUNC, dtype}, agg_type(t), arg(std::move(a)) {}
};
} // namespace simple_olap
