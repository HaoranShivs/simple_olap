#include "vector.h"

#include <algorithm>
#include <cstring>

namespace simple_olap {

// ==========================================
// ColumnData 实现
// ==========================================

bool ColumnData::is_view() const {
    return is_view_;
}

void ColumnData::Resize(uint32_t new_count) {
    const size_t bytes = TypeElemSize(type) * static_cast<size_t>(new_count);

    // 当前 Handle 容量足够：不 Acquire、不 Release，直接复用。
    if (!owned_buffer_ || owned_buffer_.capacity() < bytes) {
        owned_buffer_.Reset();
        owned_buffer_ = buffer_pool_->Acquire(bytes);
    }

    count = new_count;
    buffer = owned_buffer_.data();
    is_view_ = false;
}

void ColumnData::CopyFrom(const void* src, uint32_t elem_count, bool is_view) {
    count = elem_count;
    if (is_view) {
        buffer = const_cast<uint8_t*>(static_cast<const uint8_t*>(src));
        owned_buffer_.Reset();
        is_view_ = true;
    } else {
        const size_t bytes = TypeElemSize(type) * static_cast<size_t>(elem_count);

        if (!owned_buffer_ || owned_buffer_.capacity() < bytes) {
            owned_buffer_.Reset();
            owned_buffer_ = buffer_pool_->Acquire(bytes);
        }

        std::memcpy(owned_buffer_.data(), src, bytes);
        buffer = owned_buffer_.data();
        is_view_ = false;
    }
}

void ColumnData::Materialize() {
    if (!is_view_) {
        return;
    }

    const size_t bytes = TypeElemSize(type) * static_cast<size_t>(count);

    BufferHandle new_buffer = buffer_pool_->Acquire(bytes);
    std::memcpy(new_buffer.data(), buffer, bytes);

    owned_buffer_ = std::move(new_buffer);
    buffer = owned_buffer_.data();
    is_view_ = false;
}

void ColumnData::ReplaceWith(BufferHandle&& buffer_handle, uint32_t new_count) {
    owned_buffer_ = std::move(buffer_handle);
    buffer = owned_buffer_.data();
    count = new_count;
    is_view_ = false;
}

void ColumnData::Reset() {
    // Reset ≠ free：owned buffer 归还 BufferPool。
    owned_buffer_.Reset();

    buffer = nullptr;
    count = 0;
    is_view_ = true;
}

// ==========================================
// VectorBatch 实现
// ==========================================

VectorBatch::VectorBatch(BufferPool* buffer_pool, bool is_view) : buffer_pool_(buffer_pool), is_view_(is_view) {
    // 选择向量只 reserve 一次（约 4 KB），避免执行期反复分配。
    sel_vector.reserve(BATCH_SIZE);
}

bool VectorBatch::is_view() const {
    return is_view_;
}

uint32_t VectorBatch::ColumnCount() const {
    return static_cast<uint32_t>(columns.size());
}

void VectorBatch::AddColumn(DataType type) {
    columns.emplace_back(type, buffer_pool_);
}

void VectorBatch::Reset() {
    for (auto& col : columns) {
        col.Reset();
    }
    sel_vector.clear();
    size = 0;
}

void VectorBatch::CompactBySel() {
    if (sel_vector.empty()) {
        return; // identity selection
    }

    const uint32_t new_count = static_cast<uint32_t>(sel_vector.size());
    for (auto& col : columns) {
        const size_t elem_size = TypeElemSize(col.type);
        const size_t bytes = static_cast<size_t>(new_count) * elem_size;

        BufferHandle compact = buffer_pool_->Acquire(bytes);
        for (uint32_t i = 0; i < new_count; ++i) {
            std::memcpy(compact.data() + static_cast<size_t>(i) * elem_size,
                        col.buffer + static_cast<size_t>(sel_vector[i]) * elem_size, elem_size);
        }
        col.ReplaceWith(std::move(compact), new_count);
    }
    sel_vector.clear();
    size = new_count;
}
} // namespace simple_olap
