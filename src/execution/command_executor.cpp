#include "command_executor.h"

#include <cstring>
#include <memory>
#include <stdexcept>
#include <vector>

#include "../catalog/table_catalog_entry.h"
#include "../storage/datastructs.h"
#include "../storage/table/table_storage.h"

namespace simple_olap {

CommandResult CommandExecutor::Execute(const PhysicalPlan& plan) {
    switch (plan.GetType()) {
    case PhysicalPlan::Type::INSERT:
        return ExecuteInsert(static_cast<const PhysicalInsert&>(plan));
    case PhysicalPlan::Type::CREATE_TABLE:
        return ExecuteCreateTable(static_cast<const PhysicalCreateTable&>(plan));
    default:
        throw std::runtime_error("CommandExecutor: plan is not a command plan");
    }
}

// ==========================================
// CREATE TABLE
// ==========================================
// DDL 协调点（对应 Database::CreateTable 的职责）：
//   1. Catalog：登记逻辑元数据（name / schema / table_id）
//   2. StorageManager：创建物理存储（tables/{table_id}/ + table.meta）
//   3. 物理创建失败时回滚 Catalog 条目
CommandResult CommandExecutor::ExecuteCreateTable(const PhysicalCreateTable& plan) {
    // 1. 物理计划 -> Catalog 语句（column_id 按列顺序从 0 分配，与 Catalog::CreateTable 约定一致）
    CreateTableStatement stmt;
    stmt.table_name = plan.GetTableName();

    const auto& columns = plan.GetColumns();
    stmt.columns.reserve(columns.size());
    for (uint32_t i = 0; i < static_cast<uint32_t>(columns.size()); ++i) {
        stmt.columns.push_back(ColumnSchema{i, columns[i].name, columns[i].type});
    }

    // 2. Catalog：登记元数据
    if (!ctx_->catalog->CreateTable(stmt)) {
        return CommandResult{false, 0};
    }

    // 3. StorageManager：创建物理存储
    const TableId table_id = ctx_->catalog->FindTable(stmt.table_name).value();
    const TableCatalogEntry* entry = ctx_->catalog->GetTable(table_id);
    const auto storage = ctx_->storage_manager->CreateTable(table_id, entry->schema);
    if (storage == nullptr) {
        // 物理创建失败：回滚 Catalog 条目
        ctx_->catalog->DropTable(stmt.table_name);
        return CommandResult{false, 0};
    }

    return CommandResult{true, 0};
}

// ==========================================
// INSERT
// ==========================================
// 常量表达式求值：INSERT VALUES 只允许常量（字面量与常量算术），
// 出现 COLUMN_REF / AGG_FUNC 属于 binder 语义错误，这里直接抛内部错误。
// 数值结果以 double 承载；字符串字面量以 std::string 承载（VARCHAR 列专用）。
struct InsertValue {
    bool is_string = false;
    double number = 0.0;
    std::string text;
};

static InsertValue EvalInsertConstant(const PlanExpr& expr) {
    switch (expr.GetType()) {
    case PlanExpr::Type::LITERAL: {
        const auto& value = static_cast<const PlanLiteral&>(expr).GetValue();
        if (std::holds_alternative<int32_t>(value)) {
            return InsertValue{false, static_cast<double>(std::get<int32_t>(value)), ""};
        }
        if (std::holds_alternative<double>(value)) {
            return InsertValue{false, std::get<double>(value), ""};
        }
        return InsertValue{true, 0.0, std::get<std::string>(value)};
    }
    case PlanExpr::Type::BINARY_OP: {
        const auto& binary = static_cast<const PlanBinaryExpr&>(expr);
        const InsertValue lhs = EvalInsertConstant(binary.GetLeft());
        const InsertValue rhs = EvalInsertConstant(binary.GetRight());
        if (lhs.is_string || rhs.is_string) {
            throw std::runtime_error("INSERT: arithmetic on string literal is not supported");
        }
        switch (binary.GetOp()) {
        case PlanBinaryOp::ADD:
            return InsertValue{false, lhs.number + rhs.number, ""};
        case PlanBinaryOp::SUB:
            return InsertValue{false, lhs.number - rhs.number, ""};
        default:
            throw std::runtime_error("INSERT: unsupported operator in VALUES expression");
        }
    }
    default:
        throw std::runtime_error("INSERT VALUES must be constant expressions");
    }
}

CommandResult CommandExecutor::ExecuteInsert(const PhysicalInsert& plan) {
    // 1. bind 阶段已把表名解析成 table_id；schema 权威来源在 Catalog
    const TableId table_id = plan.GetTableOid();
    const TableCatalogEntry* entry = ctx_->catalog->GetTable(table_id);
    if (entry == nullptr) {
        throw std::runtime_error("INSERT: table " + std::to_string(table_id) + " not found in catalog");
    }
    const TableSchema& schema = entry->schema;

    const auto& rows = plan.GetRows();
    const auto& target_columns = plan.GetTargetColumns();
    const size_t num_rows = rows.size();

    // 2. 分配列式缓冲：DataChunk 按 schema 列顺序布局
    //    （SegmentBuilder::Append 按 schema 顺序逐列读取 batch.data<uint8_t>(i)）
    const size_t num_schema_cols = schema.columns.size();
    std::vector<std::shared_ptr<uint8_t[]>> buffers(num_schema_cols);
    std::vector<size_t> elem_sizes(num_schema_cols);

    for (size_t c = 0; c < num_schema_cols; ++c) {
        const DataType type = schema.columns[c].type;
        if (type == DataType::INVALID) {
            throw std::runtime_error("INSERT: unsupported column type for column " + schema.columns[c].name);
        }
        elem_sizes[c] = TypeElemSize(type);
        buffers[c].reset(new uint8_t[num_rows * elem_sizes[c]]);
        // 未出现在 target_columns 中的列保持零值（暂不支持 DEFAULT 语义）
        std::memset(buffers[c].get(), 0, num_rows * elem_sizes[c]);
    }

    // 3. 逐行求值并写入目标列
    for (size_t r = 0; r < num_rows; ++r) {
        const auto& row = rows[r];
        if (row.size() != target_columns.size()) {
            throw std::runtime_error("INSERT: row width mismatch with target columns");
        }

        for (size_t i = 0; i < row.size(); ++i) {
            const uint32_t col_idx = target_columns[i];
            if (col_idx >= num_schema_cols) {
                throw std::runtime_error("INSERT: target column index out of range");
            }

            const InsertValue value = EvalInsertConstant(*row[i]);
            uint8_t* dst = buffers[col_idx].get() + r * elem_sizes[col_idx];

            switch (schema.columns[col_idx].type) {
            case DataType::INT32:
                if (value.is_string) {
                    throw std::runtime_error("INSERT: string literal for numeric column " +
                                             schema.columns[col_idx].name);
                }
                *reinterpret_cast<int32_t*>(dst) = static_cast<int32_t>(value.number);
                break;
            case DataType::INT64:
                if (value.is_string) {
                    throw std::runtime_error("INSERT: string literal for numeric column " +
                                             schema.columns[col_idx].name);
                }
                *reinterpret_cast<int64_t*>(dst) = static_cast<int64_t>(value.number);
                break;
            case DataType::FLOAT:
                if (value.is_string) {
                    throw std::runtime_error("INSERT: string literal for numeric column " +
                                             schema.columns[col_idx].name);
                }
                *reinterpret_cast<float*>(dst) = static_cast<float>(value.number);
                break;
            case DataType::DOUBLE:
                if (value.is_string) {
                    throw std::runtime_error("INSERT: string literal for numeric column " +
                                             schema.columns[col_idx].name);
                }
                *reinterpret_cast<double*>(dst) = value.number;
                break;
            case DataType::VARCHAR:
                if (!value.is_string) {
                    throw std::runtime_error("INSERT: numeric literal for VARCHAR column " +
                                             schema.columns[col_idx].name);
                }
                WriteVarcharSlot(dst, value.text);
                break;
            default:
                throw std::runtime_error("INSERT: unsupported column type for column " + schema.columns[col_idx].name);
            }
        }
    }

    // 4. 组装 DataChunk 并交给 TableStorage
    DataChunk chunk(num_rows);
    for (size_t c = 0; c < num_schema_cols; ++c) {
        chunk.set_data(c, buffers[c], elem_sizes[c]);
    }

    auto storage = ctx_->storage_manager->GetTable(table_id, schema);
    if (storage == nullptr) {
        throw std::runtime_error("INSERT: failed to open physical storage for table " + std::to_string(table_id));
    }

    storage->Append(chunk);

    return CommandResult{true, static_cast<uint64_t>(num_rows)};
}

} // namespace simple_olap
