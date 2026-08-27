#include "columnchunk.h"

#include <cstring>

#include "../file/filewriter.h"

namespace simple_olap
{

    // ==========================================
    // ColumnBuilder - 列数据写入器
    // ==========================================

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

    // 把已积累的列数据写入 writer 当前位置，返回是否成功
    bool ColumnBuilder::Flush(FileWriter &writer)
    {
        if (data_.empty())
        {
            return true;
        }
        return writer.Write(data_.data(), data_.size());
    }

} // namespace simple_olap
