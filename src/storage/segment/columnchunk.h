#pragma once

#include <memory>
#include <cstdint>
#include <cstring>
#include <vector>
#include <filesystem>
#include "../datastructs.h"

namespace simple_olap
{
    // 前置声明：文件写入器（待实现）
    class FileWriter;

    using ColumnChunkId = uint32_t;

    class ColumnChunkReader
    {
    public:
        const ColumnChunkMeta &metadata() const { return metadata_; }

        const std::byte *data() const noexcept { return data_; }

        // 静态工厂方法：从 SegmentBuilder 的数据创建 ColumnChunk
        static ColumnChunkReader CreateFromBuilder(
            const ColumnChunkMeta &meta,
            const uint8_t *data,
            uint32_t row_count,
            size_t element_size)
        {
            (void)row_count;
            (void)element_size;
            // 不拷贝数据，只持有指针（数据由 SegmentBuilder 的 column_builders_ 管理生命周期）
            ColumnChunkReader chunk(meta,
                              reinterpret_cast<const std::byte *>(data),
                              nullptr);
            // data_owner_ 为空——数据由 SegmentBuilder 的 column_builders_ 管理
            return chunk;
        }

    private:
        ColumnChunkReader(const ColumnChunkMeta &meta,
                    const std::byte *data,
                    const std::byte *aux_data) noexcept
            : metadata_(meta), data_(data), aux_data_(aux_data)
        {
        }

        // 数据所有权：由 shared_ptr 管理生命周期（可选，用于延长数据寿命）
        std::shared_ptr<const uint8_t[]> data_owner_;

        const ColumnChunkMeta metadata_;

        const std::byte *data_;
        const std::byte *aux_data_;
    };

    // 列向量视图：指向 DataChunk 中某列数据的轻量描述
    // 不拥有数据，仅记录数据指针、元素大小和行数
    // 行数记录是为了给 datachunk 的 slice 功能打补丁
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

        bool Flush(FileWriter &writer);

        // 获取列数据缓冲指针（只读）
        const uint8_t *GetData() const { return data_.data(); }

        // 获取当前行数
        uint32_t row_count() const { return row_count_; }

        // 获取元数据
        const ColumnChunkMeta &GetMeta() const { return metadata_; }

        // 获取元素大小（字节），TypeElemSize 定义于 type.h
        size_t element_size() const { return TypeElemSize(metadata_.type); }

    private:
        ColumnChunkMeta metadata_;
        uint32_t row_count_;
        std::vector<uint8_t> data_; // 列数据缓冲，随 Append 动态增长
    };

} // namespace simple_olap
