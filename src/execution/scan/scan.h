#pragma once

#include "../operator.h"
#include "../../storage/table/table.h"
#include "../vector/vector.h"
#include <string>
#include <vector>

namespace simple_olap
{
    /// @brief 全表顺序扫描算子
    /// 从 Table 按列拷贝数据到 VectorBatch，每次产出一个 Batch。
    class TableScan : public Operator
    {
    public:
        TableScan(const Table *table, std::vector<uint32_t> column_ids);

        void Init() override;
        bool Next(VectorBatch &batch) override;

    private:
        static size_t TypeSize(DataType type); // 这个还没有实现

        const Table *table_;
        std::vector<uint32_t> column_ids_;
        size_t current_offset_ = 0;
        size_t total_rows_ = 0;
        bool schema_resolved_ = false;
    };
} // namespace simple_olap