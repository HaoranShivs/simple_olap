#pragma once

#include <cstdint>
#include <vector>

#include "../../type.h"     // TableId
#include "../datastructs.h" // SegmentId / serialization

namespace simple_olap {

// 单表物理存储元数据：只描述"数据在哪里、由哪些 segment 组成"。
// 逻辑信息（name / schema）由 catalog/table_catalog_entry.h 的
// TableCatalogEntry 负责，两者通过 TableId 关联。
struct TableStorageMeta {
    TableId table_id = 0;

    // 已落盘 segment 的 id 列表（文件命名格式固定：{segment_id}）
    std::vector<SegmentId> segment_ids;

    // 将来可加入：uint64_t row_count;

    void Serialize(BinaryWriter& writer) const {
        writer.WriteUInt32(table_id);
        writer.WriteUInt32(static_cast<uint32_t>(segment_ids.size()));
        for (SegmentId id : segment_ids) {
            writer.WriteUInt32(id);
        }
    }

    static TableStorageMeta Deserialize(BinaryReader& reader) {
        TableStorageMeta meta;
        meta.table_id = reader.ReadUInt32();
        uint32_t count = reader.ReadUInt32();
        meta.segment_ids.reserve(count);
        for (uint32_t i = 0; i < count; ++i) {
            meta.segment_ids.push_back(reader.ReadUInt32());
        }
        return meta;
    }
};

} // namespace simple_olap
