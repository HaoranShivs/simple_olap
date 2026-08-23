#pragma once

#include <cstdint>
#include <vector>
#include <memory>

namespace simple_olap
{

    // ==========================================
    // DataChunk - 数据块
    // ==========================================
    // 用于批量数据传输

    class DataChunk
    {
    public:
        explicit DataChunk(size_t capacity = 1024);

        size_t size() const { return size_; }
        void set_size(size_t size) { size_ = size; }
        void reset() { size_ = 0; }

        // 获取指定列的数据指针
        template <typename T>
        T *data(size_t col_idx)
        {
            return reinterpret_cast<T *>(chunks_[col_idx].get());
        }

        // 设置指定列的数据
        void set_data(size_t col_idx, std::unique_ptr<uint8_t[]> data, size_t element_size);

    private:
        size_t size_;
        std::vector<std::unique_ptr<uint8_t[]>> chunks_;
        std::vector<size_t> element_sizes_;
    };

} // namespace simple_olap
