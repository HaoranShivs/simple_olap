#pragma once

#include <string>

#include "../storage/datastructs.h" // TableSchema / ColumnSchema
#include "../storage/serialization.h"
#include "../type.h" // TableId

namespace simple_olap {

// 逻辑表的目录条目：Catalog 只关心"这张表是什么"。
// 物理存储（segment 布局、行数等）由 storage/table_storage_meta.h 的
// TableStorageMeta 负责，两者通过 TableId 关联，互不持有对方。
struct TableCatalogEntry {
    TableId table_id = 0; // 表 OID，由 Catalog 分配
    std::string name;     // 表名（catalog 内唯一）
    TableSchema schema;   // 逻辑 schema：列名、类型、column_id

    // 序列化 / 反序列化在条目内部实现，catalog.cpp 只负责文件读写
    void Serialize(BinaryWriter& writer) const {
        writer.WriteString(name);
        writer.WriteUInt32(table_id);
        schema.Serialize(writer);
    }

    static TableCatalogEntry Deserialize(BinaryReader& reader) {
        TableCatalogEntry entry;
        entry.name = reader.ReadString();
        entry.table_id = reader.ReadUInt32();
        entry.schema = TableSchema::Deserialize(reader);
        return entry;
    }
};

} // namespace simple_olap
