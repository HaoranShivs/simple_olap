#pragma once

#include "../datastructs.h"
#include "../datachunk.h"

namespace simple_olap
{
    using SegmentId = uint32_t;
    using ColumnChunkId = uint32_t;
    /* 被GPT强力否定，按照它讲的，换成以下结构
    class ColumnChunk
    {
    public:
        // 创建新ColumnChunk，给创建segment留的底层接口
        // bool Create(const ColumnChunk &metadata);

        // 从硬盘载入
        bool Load(int64_t offset);

        // 写回硬盘
        // bool Flush();

    private:
        ColumnChunkMeta metadata_;
    };
    */

    class ColumnChunk
    {
    public:
        const ColumnChunkMeta &metadata() const;

        const std::byte *data() const noexcept;

    private:
        const ColumnChunkMeta *metadata_;

        const std::byte *data_;
        const std::byte *aux_data_;
    };

    class Segment
    {
    public:
        // 创建新Segment
        // bool Create(const SegmentMeta &metadata, const std::filesystem::path &root_dir);

        // 从硬盘引入Segment
        bool Open(SegmentId id, const std::filesystem::path &root_dir);

        // 将Segment写回硬盘
        // bool Flush();

        uint64_t id() const;

        uint32_t row_count() const;

        uint32_t column_count() const;

        // // 获取表的scaner
        // std::unique_ptr<class SegmentScaner> Scan(const ScanOptions &options) const;
        // 写内容不在segment内部，而是外部重构segment，直接整个替换。

        const ColumnChunkMeta &GetColumnMeta(ColumnId id) const noexcept;

        ColumnChunk OpenColumn(uint32_t column_id) const;

    private:
        Segment(uint64_t segment_id, SegmentMeta matedata);

        uint64_t segment_id_;

        SegmentMeta metadata_;

        std::unordered_map<ColumnChunkId, ColumnChunk *> columns_id_ptr_;
    };

    class ColumnBuilder
    {
    public:
        explicit ColumnBuilder(const ColumnChunkMeta &metadata);

        void Append(const ColumnVector &column, uint32_t row_count);

        ColumnChunkMeta Flush(FileWriter &writer);

    private:
        uint32_t row_count_;
    };

    class SegmentBuilder
    {
    public:
        explicit SegmentBuilder(const TableSchema &schema);

        void Append(const DataChunk &batch);

        SegmentMeta Flush(
            const std::filesystem::path &path);

    private:
        uint32_t row_count_;

        std::vector<ColumnBuilder> column_builders_;
    };

} // namespace simple_olap
