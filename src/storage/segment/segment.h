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

    // 列向量视图：指向 DataChunk 中某列数据的轻量描述
    // 不拥有数据，仅记录数据指针、元素大小和行数
    // 行数记录是为了给datachunk的 slince功能打补丁
    struct ColumnVector
    {
        const uint8_t *data;
        size_t element_size;
        uint32_t row_count;
    };

    class ColumnBuilder
    {
    public:
        explicit ColumnBuilder(const ColumnChunkMeta &metadata);

        void Append(const ColumnVector &column);

        ColumnChunkMeta Flush(FileWriter &writer);

    private:
        ColumnChunkMeta metadata_;
        uint32_t row_count_;
        std::vector<uint8_t> data_; // 列数据缓冲，随 Append 动态增长
    };

    class SegmentBuilder
    {
    public:
        explicit SegmentBuilder(const TableSchema &schema);

        void Append(const DataChunk &batch);

        uint32_t row_count() const { return row_count_; }

        SegmentMeta Flush(
            const std::filesystem::path &path);

    private:
        uint32_t row_count_;

        std::vector<ColumnBuilder> column_builders_;
    };

} // namespace simple_olap
