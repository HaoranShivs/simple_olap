#include "datachunk.h"

namespace simple_olap
{
    // capacity 即行数：构造时 size_ 直接取该值
    DataChunk::DataChunk(size_t capacity)
        : size_(capacity), slice_start_(0)
    {
    }

    void DataChunk::set_data(size_t col_idx, std::shared_ptr<uint8_t[]> data, size_t element_size)
    {
        // 按需扩容到 col_idx（列按 0..n-1 顺序填充）
        if (col_idx >= chunks_.size())
        {
            chunks_.resize(col_idx + 1);
            element_sizes_.resize(col_idx + 1);
        }
        chunks_[col_idx] = std::move(data);
        element_sizes_[col_idx] = element_size;
    }

} // namespace simple_olap
