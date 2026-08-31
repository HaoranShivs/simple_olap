#pragma once

#include "datastructs.h"

namespace simple_olap
{
    struct ColumnDefinition
    {
        std::string name;
        DataType type;
    };

    struct CreateTableStatement
    {
        std::string table_name;
        std::vector<ColumnDefinition> columns;
    };

    // CREATE TABLE 
    struct ColumnDefinition
    {
        std::string name;
        DataType type;
    };

    struct CreateTableStatement
    {
        std::string table_name;
        std::vector<ColumnDefinition> columns;
    };

    // INSERT
    using LiteralValue =
        std::variant<
            int32_t,
            int64_t,
            double,
            std::string>;

    struct InsertStatement
    {
        std::string table_name;

        std::vector<
            std::vector<LiteralValue>>
            rows;
    };

    enum class AggType: uint8_t{
        INVALID = 0,
        SUM,    // 求和
        COUNT,  // 计数
        AVG,    // 平均值（内部用 SUM + COUNT 实现）
        MIN,    // 最小值
        MAX     // 最大值 
    };

    struct SelectItem{
        std::string column_name;
        AggType op;  
    };

    // CmpOp 定义于 datastructs.h

    struct Expression{
        std::string column_name;
        CmpOp op;
        int32_t value;
    };

    // SELECT
    struct SelectStatement
    {
        std::string table_name;

        std::vector<SelectItem> select_list;

        Expression* where;

        std::vector<Expression*>
            group_by;
    };

    // ---------- ID-based versions of SELECT statements ----------

    struct SelectTarget
    {
        ColumnId column_id;
        AggType op;
    };

    struct ExprTarget
    {
        ColumnId column_id;
        CmpOp op;
        int32_t value;
    };

    struct SelectTargetStatement
    {
        TableId table_id;

        std::vector<SelectTarget> target_list;

        std::unique_ptr<ExprTarget> where;

        std::vector<std::unique_ptr<ExprTarget>> group_by;
    };

    // 然后关键来了：

    using StatementData =
        std::variant<
            CreateTableStatement,
            InsertStatement,
            SelectStatement>;

    struct Statement
    {
        StatementData data;
    };

} // namespace simple_olap
