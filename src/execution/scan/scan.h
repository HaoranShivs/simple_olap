#pragma once

#include "../operator.h"
#include "../../storage/table/table.h"
#include "../vector/vector.h"
#include "../../storage/catalog.h"
#include "../../storage/test_statement.h"
#include <string>
#include <vector>

namespace simple_olap
{
    // ScanCursor / Condition / ScanOptions 定义于 storage/datastructs.h

    /// @brief 全表顺序扫描算子
    /// 从 Table 按列拷贝数据到 VectorBatch，每次产出一个 Batch。
    // !!!有缺陷，不能视图绑定，只能深拷贝。
    class TableScan : public Operator
    {
    public:
        TableScan(const SelectStatement &selectstat, Catalog *catalog);

        // 查看表是否存在，并绑定表。
        void Init() override;

        bool Next(VectorBatch &batch) override;

        // 私有辅助：按列名在 schema 中查找 column_id；未找到抛出异常
        static ColumnId FindColumnId(const TableSchema &schema,
                                     const std::string &column_name);

    private:
        // 初始化
        const SelectStatement selectstat_;
        // GetTable 是非 const 方法（可能触发 LoadTable），因此不能是 const 指针
        Catalog *catalog_;
        // Table::GetVectorBatch 是非 const 方法，因此不能是 const 指针
        Table *table_;
        // 扫描过程
        ScanCursor cursor_; // 负责记录表扫描的位置，方便多次next扫描
        // Init() 中由 selectstat_ 绑定生成（列名 -> column_id），Next() 直接复用，
        // 避免每批数据都重复转换
        SelectTargetStatement request_;
    };
} // namespace simple_olap