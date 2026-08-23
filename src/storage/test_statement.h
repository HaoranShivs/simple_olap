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

} // namespace simple_olap
