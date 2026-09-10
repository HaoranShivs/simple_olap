#pragma once

#include <memory>
#include <string>
#include <vector>

#include "../../storage/datastructs.h"
#include "expression.h"

namespace simple_olap {

// 前向声明
class StatementVisitor; // 用于执行器遍历

// SELECT 列表中的单个投影项。
struct SelectItem {
    ExprPtr expr;      // 可以是 ColumnRef, AggFunc, 甚至是 BinaryOp (a+b)
    std::string alias; // 别名，为空则使用默认名

    explicit SelectItem(ExprPtr expr, std::string alias = "") : expr(std::move(expr)), alias(std::move(alias)) {}
};

/// @brief 语句基类
struct Statement {
    // 语句类型：用枚举 + switch 分派，避免 RTTI 开销（引擎对性能敏感）。
    enum class Type {
        SELECT,
        INSERT,
        UPDATE, // 预留，暂未实现
        DELETE, // 预留，暂未实现
        CREATE_TABLE,
        EXPLAIN // 预留：打印执行计划
    };

    explicit Statement(Type type) : type_(type) {}
    virtual ~Statement() = default; // 多态基类必须有虚析构函数，防止内存泄漏

    // 获取语句类型。
    Type GetType() const {
        return type_;
    }

    // 调试接口：把 AST 还原成可读字符串，用于日志、报错信息与 EXPLAIN。
    virtual std::string ToString() const = 0;

    // 访问者模式接口（预留）。
    virtual void Accept(StatementVisitor* visitor) const = 0;

  protected:
    Type type_;
};

using StatementPtr = std::unique_ptr<Statement>;

// SELECT 语句。
struct SelectStatement : public Statement {
    std::string table_name;
    std::vector<SelectItem> select_list;

    ExprPtr where_clause;          // WHERE 条件，支持 a > 1 AND b = 2 等复合表达式
    std::vector<ExprPtr> group_by; // GROUP BY 表达式，通常为列引用

    // 预留扩展（暂不支持）：
    // ExprPtr having_clause;
    // std::vector<OrderByItem> order_by;

    SelectStatement() : Statement(Type::SELECT) {}

    std::string ToString() const override;
    void Accept(StatementVisitor* visitor) const override;
};

// INSERT 语句。
class InsertStatement : public Statement {
  public:
    InsertStatement() : Statement(Type::INSERT) {}

    std::string table_name;

    // 显式指定的列名，例如：INSERT INTO t (a, b) VALUES ...
    // 如果为空，表示插入所有列：INSERT INTO t VALUES ...
    std::vector<std::string> columns;

    // 插入的值列表：外层 vector 代表多行，内层 vector 代表单行的多个值
    // 使用 ExprPtr 可以支持字面量、表达式甚至函数调用
    std::vector<std::vector<ExprPtr>> values;

    std::string ToString() const override {
        std::string sql = "INSERT INTO " + table_name;

        if (!columns.empty()) {
            sql += " (";
            for (size_t i = 0; i < columns.size(); ++i) {
                sql += columns[i];
                if (i < columns.size() - 1)
                    sql += ", ";
            }
            sql += ")";
        }

        sql += " VALUES ";
        for (size_t i = 0; i < values.size(); ++i) {
            sql += "(";
            for (size_t j = 0; j < values[i].size(); ++j) {
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

    void Accept(StatementVisitor* visitor) const override {
        // visitor->Visit(*this);
    }
};

// CREATE TABLE 语句。
class CreateTableStatement : public Statement {
  public:
    CreateTableStatement() : Statement(Type::CREATE_TABLE) {}

    std::string table_name;
    bool if_not_exists = false; // 支持 CREATE TABLE IF NOT EXISTS

    // 列定义列表（值语义存储，内存连续，访问快）。
    std::vector<ColumnSchema> columns;

    // 暂不支持：OLAP 排序键 (Sort Key)，保留设计草稿。
    // 数据按排序键物理排序，可加速带这些列前缀的 WHERE 过滤与 GROUP BY。
    // std::vector<std::string> sort_keys;

    // 暂不支持：分区键 (Partition Key)，保留设计草稿。
    // std::vector<std::string> partition_keys;

    // 基类纯虚函数实现。
    std::string ToString() const override {
        std::string sql = "CREATE TABLE ";
        if (if_not_exists)
            sql += "IF NOT EXISTS ";
        sql += table_name + " (\n";

        static const char* type_names[] = {"INVALID", "INT32", "INT64", "FLOAT", "DOUBLE", "VARCHAR"};
        for (size_t i = 0; i < columns.size(); ++i) {
            const auto& col = columns[i];
            sql += "  " + col.name + " ";
            auto t = static_cast<int>(col.type);
            sql += (t >= 0 && t < 6) ? type_names[t] : "UNKNOWN";
            if (i < columns.size() - 1)
                sql += ",\n";
        }
        sql += "\n)";

        // 排序键 (sort_keys) 暂不支持，待启用成员后在此打印

        return sql;
    }

    void Accept(StatementVisitor* visitor) const override {
        // 后续实现 Visitor 模式时补充
        // visitor->Visit(*this);
    }
};

} // namespace simple_olap
