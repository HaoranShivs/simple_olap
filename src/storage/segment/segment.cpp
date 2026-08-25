#include "segment.h"

#include <cstring>

namespace simple_olap
{
    ColumnBuilder::ColumnBuilder(const ColumnChunkMeta &metadata)
        : metadata_(metadata), row_count_(0)
    {
    }

    void ColumnBuilder::Append(const ColumnVector &column)
    {
        const size_t byte_count = column.element_size * column.row_count;

        // 动态增长缓冲，把新数据追加到末尾
        const size_t old_size = data_.size();
        data_.resize(old_size + byte_count);
        std::memcpy(data_.data() + old_size, column.data, byte_count);

        row_count_ += column.row_count;
    }

    SegmentBuilder::SegmentBuilder(const TableSchema &schema)
        : row_count_(0)
    {
        // 为 schema 中的每一列创建一个 ColumnBuilder
        column_builders_.reserve(schema.columns.size());
        for (const auto &col : schema.columns)
        {
            ColumnChunkMeta chunk_meta;
            chunk_meta.column_id = col.column_id;
            chunk_meta.type = col.type;
            chunk_meta.data_offset = 0;
            column_builders_.emplace_back(chunk_meta);
        }
    }

    void SegmentBuilder::Append(const DataChunk &batch)
    {
        const uint32_t row_count = static_cast<uint32_t>(batch.size());

        // 为每一列构造 ColumnVector 视图，交给对应的 ColumnBuilder
        for (size_t i = 0; i < column_builders_.size(); ++i)
        {
            ColumnVector vector;
            //其实可以直接转成目标类型，不过为了解决 datachunk 中 slince 方法的问题，这里还是保持 unint8 了
            vector.data = batch.data<uint8_t>(i);   
            vector.element_size = batch.element_size(i);
            vector.row_count = row_count;
            column_builders_[i].Append(vector);
        }

        row_count_ += row_count;
    }

} // namespace simple_olap