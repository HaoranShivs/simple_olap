#pragma once

#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

#include "../execution/vector/vector.h"
#include "../sql/ast/boundstat.h"
#include "../sql/ast/statement.h"
#include "../type.h"

namespace simple_olap {

struct ResultColumn {
    std::string name;
    DataType type;
};

class QueryResult {
  public:
    enum class Type { SELECT, INSERT, CREATE_TABLE };

    Type type;

    std::vector<ResultColumn> columns;

    // SELECT result
    std::vector<VectorBatch> chunks;

    // INSERT / DDL
    uint64_t affected_rows = 0;

    // 追加一个 batch：深拷贝各列数据（batch 可能是视图模式，
    // 指向 mmap 区或算子内部缓冲，回调返回后即失效，必须物化）。
    // 同时累加 affected_rows 作为总行数。
    void Append(const VectorBatch& batch) {
        VectorBatch copy(/*is_view=*/false);

        copy.columns.reserve(batch.columns.size());
        for (const auto& col : batch.columns) {
            ColumnData column;
            column.type = col.type;
            column.count = col.count;
            column.CopyFrom(col.buffer, col.count, /*is_view=*/false);
            copy.columns.push_back(std::move(column));
        }

        copy.sel_vector = batch.sel_vector;
        copy.size = batch.size;

        chunks.push_back(std::move(copy));
        affected_rows += batch.size;
    }

    // 从 (parsed AST, bound AST) 更新本对象的 type / columns：
    //   type    由 bound 语句类型决定
    //   columns 由 bound select_list 推导（类型来自 Binder，名称优先 AS alias，
    //           无 alias 则回退到原始 SQL 表达式文本）
    // INSERT / CREATE_TABLE 只更新 type，不产生列。
    void BuildResultMetadata(const Statement& statement, const BoundStatement& bound_statement) {
        switch (bound_statement.GetType()) {
        case BoundStatement::Type::SELECT: {
            type = Type::SELECT;

            const auto& parsed = static_cast<const SelectStatement&>(statement);
            const auto& bound = static_cast<const BoundSelectStatement&>(bound_statement);

            // SELECT * 展开：bound 侧是按 schema 展开的全部列，
            // 与 parsed 侧的单个 "*" 项不是 1:1 对应。
            // 此时列名直接取 Binder 填好的 alias（即列名），类型取 bound 表达式。
            const bool is_star = parsed.select_list.size() == 1 && parsed.select_list[0].expr->ToString() == "*";

            if (!is_star && parsed.select_list.size() != bound.select_list.size()) {
                throw std::logic_error("parsed/bound select list size mismatch");
            }

            columns.clear();
            columns.reserve(bound.select_list.size());
            for (size_t i = 0; i < bound.select_list.size(); ++i) {
                const auto& bound_item = bound.select_list[i];

                ResultColumn column;

                // 类型必须从 Binder 获取
                column.type = bound_item.expr->return_type;

                // 名称优先使用 AS alias（SELECT * 展开时 Binder 已填列名）
                if (!bound_item.alias.empty()) {
                    column.name = bound_item.alias;
                } else if (is_star) {
                    throw std::logic_error("SELECT * expansion missing column alias");
                } else {
                    // 没 alias 则使用原始 SQL 表达式
                    column.name = parsed.select_list[i].expr->ToString();
                }

                columns.push_back(std::move(column));
            }

            break;
        }

        case BoundStatement::Type::INSERT:
            type = Type::INSERT;
            break;

        case BoundStatement::Type::CREATE_TABLE:
            type = Type::CREATE_TABLE;
            break;

        default:
            throw std::logic_error("unsupported result metadata type");
        }
    }
};

} // namespace simple_olap
