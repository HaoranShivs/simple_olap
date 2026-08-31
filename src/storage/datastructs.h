#pragma once

#include <string>
#include <unordered_map>
#include <vector>
#include <map>
#include <filesystem>
#include <variant>
#include "../type.h"
#include "serialization.h"

namespace simple_olap
{
    using TableId = uint32_t;
    using ColumnId = uint32_t;
    using SegmentId = uint32_t;

    // 单个 segment 的最大行数，活跃 segment 达到该行数后自动封存
    constexpr uint32_t kMaxSegmentRowCount = 65536;

    enum class CmpOp : uint8_t {
        EQ, NE, GT, GE, LT, LE
    };

    // 扫描游标：记录当前读取位置
    struct ScanCursor
    {
        SegmentId segment_id = 0;       // 当前 segment id
        uint32_t offset_in_segment = 0; // 在当前 segment 内的行偏移
    };

    struct Condition
    {
        ColumnId column = 0;
        CmpOp op = CmpOp::EQ;
        std::variant<int32_t, int64_t, double, std::string> value;
    };

    struct ScanOptions
    {
        uint64_t start_row = 0;        // 起始行
        uint64_t end_row = UINT64_MAX; // 结束行
        std::vector<ColumnId> columns; // 需要读取的列，空表示全部
        // 过滤条件；has_where 为 false 时忽略 cond
        bool has_where = false;
        Condition cond;
    };

    struct ColumnSchema
    {
        uint32_t column_id;
        std::string name;
        DataType type;

        void Serialize(BinaryWriter &writer) const
        {
            writer.WriteUInt32(column_id);
            writer.WriteString(name);
            writer.WriteUInt8(static_cast<uint8_t>(type));
        }

        static ColumnSchema Deserialize(BinaryReader &reader)
        {
            ColumnSchema schema;
            schema.column_id = reader.ReadUInt32();
            schema.name = reader.ReadString();
            schema.type = static_cast<DataType>(reader.ReadUInt8());
            return schema;
        }
    };

    struct TableSchema
    {
        std::vector<ColumnSchema> columns;

        void Serialize(BinaryWriter &writer) const
        {
            writer.WriteUInt32(static_cast<uint32_t>(columns.size()));
            for (const auto &col : columns)
            {
                col.Serialize(writer);
            }
        }

        static TableSchema Deserialize(BinaryReader &reader)
        {
            TableSchema schema;
            uint32_t count = reader.ReadUInt32();
            schema.columns.reserve(count);
            for (uint32_t i = 0; i < count; ++i)
            {
                schema.columns.push_back(ColumnSchema::Deserialize(reader));
            }
            return schema;
        }
    };

    struct CatalogMeta
    {
        // 暂时不设置用户，版本，权限等信息。
        std::map<std::string, uint32_t> table_name_id;

        void Serialize(BinaryWriter &writer) const
        {
            writer.WriteUInt32(static_cast<uint32_t>(table_name_id.size()));
            for (const auto &kv : table_name_id)
            {
                writer.WriteString(kv.first);
                writer.WriteUInt32(kv.second);
            }
        }

        static CatalogMeta Deserialize(BinaryReader &reader)
        {
            CatalogMeta meta;
            uint32_t count = reader.ReadUInt32();
            for (uint32_t i = 0; i < count; ++i)
            {
                std::string name = reader.ReadString();
                uint32_t id = reader.ReadUInt32();
                meta.table_name_id[name] = id;
            }
            return meta;
        }
    };

    struct TableMeta
    {
        uint32_t table_id;
        std::string name; // table层面不需要知道自己的名字吗？实际上不需要，但是有的话会很方便

        TableSchema schema;

        // 已封存 segment 的 id 列表（文件命名格式固定，无需记录全名）
        std::vector<uint32_t> segment_ids;

        void Serialize(BinaryWriter &writer) const
        {
            writer.WriteUInt32(table_id);
            writer.WriteString(name);
            schema.Serialize(writer);
            writer.WriteUInt32(static_cast<uint32_t>(segment_ids.size()));
            for (uint32_t id : segment_ids)
            {
                writer.WriteUInt32(id);
            }
        }

        static TableMeta Deserialize(BinaryReader &reader)
        {
            TableMeta meta;
            meta.table_id = reader.ReadUInt32();
            meta.name = reader.ReadString();
            meta.schema = TableSchema::Deserialize(reader);
            uint32_t count = reader.ReadUInt32();
            meta.segment_ids.reserve(count);
            for (uint32_t i = 0; i < count; ++i)
            {
                meta.segment_ids.push_back(reader.ReadUInt32());
            }
            return meta;
        }
    };

    struct ColumnChunkMeta
    {
        uint32_t column_id;

        DataType type;

        uint64_t data_offset;

        // 列内数值的最大/最小值，用于 segment 级 min/max 剪枝（见 SegmentReader::GetVectorBatch）。
        // 以 double 统一存储（INT32/INT64/FLOAT/DOUBLE 均可无损或近似表示）；
        // has_stats 为 false 表示无统计信息（如 VARCHAR 列或空列），剪枝时跳过该条件。
        bool has_stats;

        double min_value;

        double max_value;

        void Serialize(BinaryWriter &writer) const
        {
            writer.WriteUInt32(column_id);
            writer.WriteUInt8(static_cast<uint8_t>(type));
            writer.WriteUInt64(data_offset);
            writer.WriteUInt8(has_stats ? 1 : 0);
            writer.WriteDouble(min_value);
            writer.WriteDouble(max_value);
        }

        static ColumnChunkMeta Deserialize(BinaryReader &reader)
        {
            ColumnChunkMeta meta;
            meta.column_id = reader.ReadUInt32();
            meta.type = static_cast<DataType>(reader.ReadUInt8());
            meta.data_offset = reader.ReadUInt64();
            meta.has_stats = reader.ReadUInt8() != 0;
            meta.min_value = reader.ReadDouble();
            meta.max_value = reader.ReadDouble();
            return meta;
        }
    };

    struct SegmentMeta
    {
        uint32_t segment_id;

        uint32_t row_count;

        std::filesystem::path path;
        
        std::vector<ColumnChunkMeta> col_chunk_metas_;

        // 键的最大值最小值，主键的稀疏索引等，不过在本项目中忽略。

        void Serialize(BinaryWriter &writer) const
        {
            writer.WriteUInt32(segment_id);
            writer.WriteUInt32(row_count);
            writer.WriteString(path.string());
            writer.WriteUInt32(static_cast<uint32_t>(col_chunk_metas_.size()));
            for (const auto &chunk : col_chunk_metas_)
            {
                chunk.Serialize(writer);
            }
        }

        static SegmentMeta Deserialize(BinaryReader &reader)
        {
            SegmentMeta meta;
            meta.segment_id = reader.ReadUInt32();
            meta.row_count = reader.ReadUInt32();
            meta.path = std::filesystem::path(reader.ReadString());
            uint32_t count = reader.ReadUInt32();
            meta.col_chunk_metas_.reserve(count);
            for (uint32_t i = 0; i < count; ++i)
            {
                meta.col_chunk_metas_.push_back(ColumnChunkMeta::Deserialize(reader));
            }
            return meta;
        }
    };
}