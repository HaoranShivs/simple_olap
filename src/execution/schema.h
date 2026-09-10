#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "../type.h"

// 执行期 schema：描述算子输出有哪些列、每列的类型与来源。
// 与 plan 阶段的 schema 不同，这里是算子之间传递数据时的列定义。
namespace simple_olap {

// 列绑定：标识某列的数据来自哪张表的哪一列，用于列裁剪与列消除。
struct ColumnBinding {
    TableId table_oid = 0;
    ColumnId column_idx = 0;

    bool operator==(const ColumnBinding& other) const {
        return table_oid == other.table_oid && column_idx == other.column_idx;
    }
};

// 执行期的一列。
// source 有值表示该列直接来自某个已有列（可回溯、可裁剪）；
// source 为空表示该列是表达式计算产生的新列（如聚合结果）。
struct ExecSlot {
    DataType type = DataType::INVALID;
    std::optional<ColumnBinding> source;
    std::string name;
};

using ExecSchema = std::vector<ExecSlot>;

// 在 schema 中查找来源为 binding 的列；找不到返回 nullopt。
inline std::optional<uint32_t> FindInputSlot(const ExecSchema& schema, const ColumnBinding& binding) {
    for (uint32_t i = 0; i < static_cast<uint32_t>(schema.size()); ++i) {
        if (schema[i].source.has_value() && *schema[i].source == binding) {
            return i;
        }
    }
    return std::nullopt;
}

} // namespace simple_olap
