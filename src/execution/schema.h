#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "../type.h"

namespace simple_olap {

struct ColumnBinding {
    TableId table_oid = 0;
    ColumnId column_idx = 0;

    bool operator==(const ColumnBinding& other) const {
        return table_oid == other.table_oid && column_idx == other.column_idx;
    }
};

struct ExecSlot {
    DataType type = DataType::INVALID;
    std::optional<ColumnBinding> source;
    std::string name;
};

using ExecSchema = std::vector<ExecSlot>;

inline std::optional<uint32_t> FindInputSlot(const ExecSchema& schema, const ColumnBinding& binding) {
    for (uint32_t i = 0; i < static_cast<uint32_t>(schema.size()); ++i) {
        if (schema[i].source.has_value() && *schema[i].source == binding) {
            return i;
        }
    }
    return std::nullopt;
}

} // namespace simple_olap
