#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "boundexpr.h"

namespace simple_olap {

// 前向声明
class BoundSelectStatement;
class BoundInsertStatement;
class BoundCreateTableStatement;
class BoundStatementVisitor;

// ---------- 绑定结果基类 ----------
class BoundStatement {
  public:
    enum class Type {
        SELECT,
        INSERT,
        CREATE_TABLE,
        EXPLAIN,
        // 未来扩展...
    };

    explicit BoundStatement(Type type) : type_(type) {}
    virtual ~BoundStatement() = default;

    Type GetType() const {
        return type_;
    }

    // 调试接口：将绑定后的计划打印出来
    virtual std::string ToString() const = 0;

    // 为执行器预留的访问者接口
    virtual void Accept(BoundStatementVisitor* visitor) const = 0;

  protected:
    Type type_;
};

using BoundStatementPtr = std::unique_ptr<BoundStatement>;

// SELECT 投影项，例如 age 或 COUNT(*) AS total。
struct BoundSelectItem {
    // 多态基类需用 unique_ptr 持有，避免对象切片。
    BndExprPtr expr;

    std::string alias; // 别名；为空时由 Executor 生成默认名

    // 构造函数：接收 expr 与 alias 的所有权。
    BoundSelectItem(BndExprPtr e, std::string a = "") : expr(std::move(e)), alias(std::move(a)) {}

    // unique_ptr 为 move-only：禁用拷贝，允许移动。
    BoundSelectItem(const BoundSelectItem&) = delete;
    BoundSelectItem& operator=(const BoundSelectItem&) = delete;

    BoundSelectItem(BoundSelectItem&&) = default;
    BoundSelectItem& operator=(BoundSelectItem&&) = default;
};

// 绑定后的完整 SELECT 语句。
class BoundSelectStatement : public BoundStatement {
  public:
    uint32_t table_oid; // 绑定的物理表 OID（暂不支持 JOIN）

    // 投影项列表。
    std::vector<BoundSelectItem> select_list;

    // WHERE 条件；不存在时为空。
    BndExprPtr where_clause;
    // GROUP BY 表达式；不存在时为空。
    std::vector<BndExprPtr> group_by;

    BoundSelectStatement(uint32_t t_oid, std::vector<BoundSelectItem> s_list, BndExprPtr where,
                         std::vector<BndExprPtr> g_by)
        : BoundStatement(Type::SELECT), table_oid(t_oid), select_list(std::move(s_list)),
          where_clause(std::move(where)), group_by(std::move(g_by)) {}

    std::string ToString() const override {
        return "BoundSelectStatement(...)"; // 占位：未输出具体内容
    }

    void Accept(BoundStatementVisitor* visitor) const override {
        // Visitor 暂未实现，空定义仅为满足链接。
        (void)visitor;
    }
};

// ---------- 其他语句的绑定结果 ----------
class BoundInsertStatement : public BoundStatement {
  public:
    BoundInsertStatement() : BoundStatement(Type::INSERT) {}
    // 目标表 OID。
    uint32_t table_oid;
    // 目标列的物理索引；未指定列名时按全列顺序 0..n-1。
    std::vector<uint32_t> target_col_indices;
    // 逐行、逐列的插入值（表达式形式）。
    std::vector<std::vector<std::unique_ptr<BoundExpr>>> values;

    std::string ToString() const override {
        return "BoundInsertStatement(...)";
    }
    void Accept(BoundStatementVisitor* visitor) const override {
        // Visitor 暂未实现，空定义仅为满足链接。
        (void)visitor;
    }
};

// 绑定后的列定义（CREATE TABLE 使用）。
struct BoundColumnDef {
    std::string name;
    DataType type;
};

// 绑定后的 CREATE TABLE 语句。
class BoundCreateTableStatement : public BoundStatement {
  public:
    BoundCreateTableStatement() : BoundStatement(Type::CREATE_TABLE) {}
    // 目标表名与列定义。
    std::string table_name;
    std::vector<BoundColumnDef> columns;

    std::string ToString() const override {
        return "BoundCreateTableStatement(...)";
    }
    void Accept(BoundStatementVisitor* visitor) const override {
        // Visitor 暂未实现，空定义仅为满足链接。
        (void)visitor;
    }
};
} // namespace simple_olap
