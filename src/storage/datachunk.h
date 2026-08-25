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
    // 支持 Slice 轻量切片：切片与原块共享底层缓冲（shared_ptr），
    // 仅通过 slice_start_（起始行）和 size_（行数）描述各自视图

    class DataChunk
    {
    public:
        explicit DataChunk(size_t capacity = 1024);

        size_t size() const { return size_; }
        void set_size(size_t size) { size_ = size; }
        void reset() { size_ = 0; }

        // 获取指定列的数据指针（已按 slice_start_ 偏移）
        template <typename T>
        T *data(size_t col_idx)
        {
            return reinterpret_cast<T *>(chunks_[col_idx].get()) + slice_start_;
        }

        // 获取指定列的数据指针（const 版，已按 slice_start_ 偏移）
        template <typename T>
        const T *data(size_t col_idx) const
        {
            return reinterpret_cast<const T *>(chunks_[col_idx].get()) + slice_start_;
        }

        // 获取指定列的元素大小
        size_t element_size(size_t col_idx) const { return element_sizes_[col_idx]; }

        // 设置指定列的数据
        void set_data(size_t col_idx, std::shared_ptr<uint8_t[]> data, size_t element_size);

        // 切片：在 boundary 行处把本块一分为二
        // this 保留 [0, boundary)，返回的视图保留 [boundary, 原size)
        // 两者共享底层缓冲，不拷贝数据
        DataChunk Slice(uint32_t boundary)
        {
            DataChunk rest = *this; // 浅拷贝，共享底层缓冲
            size_ = boundary;
            rest.slice_start_ = boundary;
            rest.size_ = rest.size_ - boundary;
            return rest;
        }

    private:
        size_t size_;
        uint32_t slice_start_ = 0; // 视图起始行（原块为 0）
        std::vector<std::shared_ptr<uint8_t[]>> chunks_;
        std::vector<size_t> element_sizes_;
    };

} // namespace simple_olap
