#pragma once
#include "../operator.h"
// #include "../../storage/table.h"
#include <string>
#include <vector>

namespace simple_olap {

/// @brief 全表顺序扫描算子
/// 从 Table 按列拷贝数据到 VectorBatch，每次产出一个 Batch。
class ScanOperator : public Operator {
public:
    ScanOperator(const Table* table, std::vector<std::string> column_names);

    void Init() override;
    bool Next(VectorBatch& batch) override;

private:
    static size_t TypeSize(TypeId type);

    const Table* table_;
    std::vector<std::string> column_names_;
    size_t current_offset_ = 0;
    size_t total_rows_ = 0;
    bool schema_resolved_ = false;
};

} // namespace simple_olap