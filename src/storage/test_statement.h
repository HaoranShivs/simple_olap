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

    // SELECT
    struct SelectStatement
    {
        std::string table_name;

        std::vector<SelectItem> select_list;

        std::unique_ptr<Expression> where;

        std::vector<std::unique_ptr<Expression>>
            group_by;
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
