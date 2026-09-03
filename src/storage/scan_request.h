#pragma once

#include <memory>
#include <vector>

#include "../type.h"
#include "datastructs.h" // TableId / ColumnId

namespace simple_olap
{
    // ---------- ID-based 扫描请求（存储层正式接口） ----------
    // 由执行层（如 TableScan）从 SQL AST 绑定生成：
    // 把列名解析为 column_id 后，交给 Table::GetVectorBatch 执行扫描。

    /// @brief 投影目标：列 id + 聚合类型
    struct SelectTarget
    {
        ColumnId column_id;
        AggType op;
    };

    /// @brief 过滤/分组目标：列 id + 比较符 + 常量值
    struct ExprTarget
    {
        ColumnId column_id;
        CmpOp op;
        int32_t value;
    };

    /// @brief ID 版 SELECT 扫描请求：Table::GetVectorBatch 的输入
    struct SelectTargetStatement
    {
        TableId table_id;

        std::vector<SelectTarget> target_list;

        std::unique_ptr<ExprTarget> where;

        std::vector<std::unique_ptr<ExprTarget>> group_by;
    };

} // namespace simple_olap
