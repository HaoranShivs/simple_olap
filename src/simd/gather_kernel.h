#pragma once

#include <cstdint>

#include "selection_mask.h"

namespace simple_olap::simd {

// ============================================================
// GatherKernels：把 SelectionMask 选中的物理行紧凑收集成连续数据
// ============================================================
//
// 语义：
//   - selection 的每个置位位（按 physical row 升序）对应一个输出行；
//   - dst 接收 selection.Count() 个连续元素；
//   - 输入 src 是完整物理列（长度 = selection.row_count()）；
//   - 不做类型转换：src/dst 类型由内核类型决定。
//
// 这是 sparse projection / CompactBySelection / 任何 late materialization
// 的唯一「sparse -> dense」实现；禁止各模块各写一份 gather 循环。

using GatherI32Fn = void (*)(const int32_t* src, const SelectionMask& selection, int32_t* dst);
using GatherI64Fn = void (*)(const int64_t* src, const SelectionMask& selection, int64_t* dst);
using GatherF32Fn = void (*)(const float* src, const SelectionMask& selection, float* dst);
using GatherF64Fn = void (*)(const double* src, const SelectionMask& selection, double* dst);

// VARCHAR：固定 64-byte slot（见 type.h），按 slot memcpy。
using GatherVarcharFn = void (*)(const uint8_t* src, const SelectionMask& selection, uint8_t* dst);

// 一组 gather 内核函数指针；scalar backend 与 avx2 backend 都填同一张表。
struct GatherKernels {
    GatherI32Fn i32 = nullptr;
    GatherI64Fn i64 = nullptr;
    GatherF32Fn f32 = nullptr;
    GatherF64Fn f64 = nullptr;
    GatherVarcharFn varchar = nullptr;
};

} // namespace simple_olap::simd
