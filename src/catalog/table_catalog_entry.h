#pragma once

#include <string>

#include "../storage/datastructs.h" // TableSchema / ColumnSchema
#include "../type.h"                // TableId

namespace simple_olap {

// 逻辑表的目录条目：Catalog 只关心"这张表是什么"。
// 物理存储（segment 布局、行数等）由 storage/table_storage_meta.h 的
// TableStorageMeta 负责，两者通过 TableId 关联，互不持有对方。
struct TableCatalogEntry {
    TableId table_id = 0;
    std::string name;
    TableSchema schema;
};

} // namespace simple_olap
