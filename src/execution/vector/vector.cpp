#include "vector.h"

#include <cstring>
#include <algorithm>

namespace simple_olap
{
    // ============ ColumnData 实现 ============

    bool ColumnData::is_view() const
    {
        return is_view_;
    }

    void ColumnData::Resize(uint32_t new_count)
    {
        count = new_count;
        const size_t elem_size = TypeElemSize(type);
        owned_buffer_.resize(elem_size * static_cast<size_t>(new_count));
        buffer = owned_buffer_.data();
        is_view_ = false;
    }

    void ColumnData::CopyFrom(const void *src, uint32_t elem_count, bool is_view)
    {
        count = elem_count;
        if (is_view)
        {
            buffer = const_cast<uint8_t *>(static_cast<const uint8_t *>(src));
            owned_buffer_.clear();
            is_view_ = true;
        }
        else
        {
            const size_t elem_size = TypeElemSize(type);
            owned_buffer_.resize(elem_size * static_cast<size_t>(elem_count));
            std::memcpy(owned_buffer_.data(), src,
                        elem_size * static_cast<size_t>(elem_count));
            buffer = owned_buffer_.data();
            is_view_ = false;
        }
    }

    void ColumnData::Materialize()
    {
        if (!is_view_)
        {
            return;
        }
        const size_t elem_size = TypeElemSize(type);
        owned_buffer_.assign(buffer, buffer + elem_size * static_cast<size_t>(count));
        buffer = owned_buffer_.data();
        is_view_ = false;
    }

    void ColumnData::ReplaceWith(std::vector<uint8_t> &&bytes, uint32_t new_count)
    {
        owned_buffer_ = std::move(bytes);
        buffer = owned_buffer_.data();
        count = new_count;
        is_view_ = false;
    }

    void ColumnData::Reset()
    {
        owned_buffer_.clear();
        buffer = nullptr;
        count = 0;
        is_view_ = false;
    }

    // ============ VectorBatch 实现 ============

    VectorBatch::VectorBatch(bool is_view)
        : is_view_(is_view)
    {
    }

    bool VectorBatch::is_view() const
    {
        return is_view_;
    }

    uint32_t VectorBatch::ColumnCount() const
    {
        return static_cast<uint32_t>(columns.size());
    }

    void VectorBatch::AddColumn(DataType type)
    {
        ColumnData col;
        col.type = type;
        columns.push_back(std::move(col));
    }

    void VectorBatch::Reset()
    {
        for (auto &col : columns)
        {
            col.Reset();
        }
        sel_vector.clear();
        size = 0;
    }

    void VectorBatch::CompactBySel()
    {
        if (sel_vector.size() == size)
        {
            return; // 全部命中，无需压缩
        }

        const uint32_t new_count = static_cast<uint32_t>(sel_vector.size());
        for (auto &col : columns)
        {
            const size_t elem_size = TypeElemSize(col.type);
            std::vector<uint8_t> compact(static_cast<size_t>(new_count) * elem_size);
            for (uint32_t i = 0; i < new_count; ++i)
            {
                std::memcpy(compact.data() + static_cast<size_t>(i) * elem_size,
                            col.buffer + static_cast<size_t>(sel_vector[i]) * elem_size,
                            elem_size);
            }
            col.ReplaceWith(std::move(compact), new_count);
        }
        size = new_count;
    }
} // namespace simple_olap
