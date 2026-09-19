#include "vector.h"

#include <algorithm>
#include <cstring>

#include "gather_utils.h"

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

VectorBatch::VectorBatch(BufferPool* buffer_pool, bool is_view) : buffer_pool_(buffer_pool), is_view_(is_view) {}

bool VectorBatch::is_view() const {
    return is_view_;
}

uint32_t VectorBatch::ColumnCount() const {
    return static_cast<uint32_t>(columns.size());
}

uint32_t VectorBatch::PhysicalSize() const noexcept {
    // selection.row_count() 是权威物理行数；未设置时退回列 count（构造中的 batch）。
    if (selection_.row_count() != 0 || columns.empty()) {
        return selection_.row_count();
    }
    return columns.front().count;
}

void VectorBatch::SetIdentitySelection(uint32_t physical_count) {
    selection_.SetAll(physical_count);
    size = physical_count;
}

void VectorBatch::SetSelection(const simd::SelectionMask& mask) {
    selection_ = mask;
    size = mask.Count();
}

void VectorBatch::CopySelectionFrom(const VectorBatch& source) {
    selection_ = source.selection_;
    size = source.size;
}

void VectorBatch::AddColumn(DataType type) {
    columns.emplace_back(type, buffer_pool_);
}

void VectorBatch::Reset() {
    for (auto& col : columns) {
        col.Reset();
    }
    columns.clear();
    selection_.SetNone(0);
    size = 0;
}

void VectorBatch::CompactBySelection() {
    if (IsDense()) {
        return; // 已经是连续有效行
    }

    const uint32_t new_count = size;

    if (new_count == 0) {
        for (auto& col : columns) {
            col.Reset();
        }
        selection_.SetNone(0);
        size = 0;
        return;
    }

    for (auto& col : columns) {
        ColumnData compact(col.type, buffer_pool_);
        compact.Resize(new_count);
        GatherColumnDense(col, selection_, compact);
        col = std::move(compact);
    }

    // 压缩后所有行连续有效：重新成为 dense batch。
    selection_.SetAll(new_count);
    size = new_count;
}
} // namespace simple_olap
