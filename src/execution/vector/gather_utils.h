#pragma once

#include <stdexcept>

#include "../../simd/kernels.h"
#include "vector.h"

namespace simple_olap {

// 按 selection 把 src 的物理行 gather 成 dst 的连续行（sparse -> dense）。
//
// 前置条件：
//   - dst 已 Resize(count) 且 dst.type == src.type
//   - count == selection.Count()
//   - selection.row_count() == src.count
//
// 这是执行期唯一的「sparse -> dense」实现：Projection / Compact 等都走这里，
// 禁止各模块各写一份 gather 循环。
inline void GatherColumnDense(const ColumnData& src, const simd::SelectionMask& selection, ColumnData& dst) {
    const simd::GatherKernels& gather = simd::KernelRegistry::Instance().gather();

    switch (src.type) {
    case DataType::INT32:
        gather.i32(src.data<int32_t>(), selection, dst.mutable_data<int32_t>());
        return;
    case DataType::INT64:
        gather.i64(src.data<int64_t>(), selection, dst.mutable_data<int64_t>());
        return;
    case DataType::FLOAT:
        gather.f32(src.data<float>(), selection, dst.mutable_data<float>());
        return;
    case DataType::DOUBLE:
        gather.f64(src.data<double>(), selection, dst.mutable_data<double>());
        return;
    case DataType::VARCHAR:
        gather.varchar(src.buffer, selection, dst.mutable_data<uint8_t>());
        return;
    default:
        throw std::runtime_error("GatherColumnDense: unsupported column type");
    }
}

} // namespace simple_olap
