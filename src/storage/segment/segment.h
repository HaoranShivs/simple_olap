#pragma once

#include <memory>
#include <unordered_map>
#include "../datachunk.h"
#include "../datastructs.h"
#include "../file/mappedfile.h"
#include "columnchunk.h"

namespace simple_olap
{
    // SegmentId / ScanOptions 定义于 ../datastructs.h
    class VectorBatch;

    class SegmentReader
    {
    public:
        // 从硬盘引入Segment：mmap 映射 segment 文件，解析元数据并建立列块视图
        // 失败返回 nullptr；成功返回的对象由调用方负责释放
        static SegmentReader* Open(SegmentId id, const std::filesystem::path &root_dir);

        ~SegmentReader();

        //--------------读取元数据--------------------

        uint64_t id() const;

        uint32_t row_count() const;

        const ColumnChunkMeta &GetColumnMeta(ColumnId id) const noexcept;

        // 大致步骤：
        // 1. 检查 scanoptions 中的where条件和对应的列的metadata，其中记录着最大值和最小值，根据这个可以判断列是否符合条件。当然不是每次运行都进行检查，只有当offset==0时才进行。
        // 2.以offset为segment内部的起点，扫描 1024（预先设置的值）行所要求列的数据到output
        bool GetVectorBatch(const ScanOptions &scanoptions, uint32_t offset, VectorBatch &output);

    private:
        SegmentReader(uint64_t segment_id, SegmentMeta matedata, MappedFile file);

        ColumnChunkReader OpenColumn(uint32_t column_id) const;

        uint64_t segment_id_;

        SegmentMeta metadata_;

        // 整个 segment 文件的只读内存映射；
        // ColumnChunkReader 的 data_ 指针直接指向映射区域，生命周期由本对象保证
        MappedFile mapped_file_;

        // 列块缓存：column_id -> ColumnChunkReader 视图
        std::unordered_map<ColumnChunkId, ColumnChunkReader> columns_;
    };

    class SegmentBuilder
    {
    public:
        explicit SegmentBuilder(const TableSchema &schema);

        void Append(const DataChunk &batch);

        bool Flush(const std::filesystem::path &path);

        uint32_t row_count() const { return row_count_; }

        // // 将自身所有列转换为 ColumnChunkReader 列表
        // std::vector<ColumnChunkReader> ToColumnChunks() const;

    private:
        uint32_t row_count_;

        std::vector<ColumnBuilder> column_builders_;
    };

} // namespace simple_olap
