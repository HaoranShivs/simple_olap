#pragma once

#include <cstdint>

#include "../type.h"
#include "selection_mask.h"

namespace simple_olap::simd {

// 类型化标量比较：scalar backend 与 AVX2 backend 的尾部共用同一份语义，
// 保证两个 backend 对所有输入结果一致（尤其覆盖 NaN 的行为）。
template <typename T> inline bool CompareTypedValue(T lhs, CmpOp op, T rhs) {
    switch (op) {
    case CmpOp::EQ:
        return lhs == rhs;
    case CmpOp::NE:
        return lhs != rhs;
    case CmpOp::GT:
        return lhs > rhs;
    case CmpOp::GE:
        return lhs >= rhs;
    case CmpOp::LT:
        return lhs < rhs;
    case CmpOp::LE:
        return lhs <= rhs;
    }
    return false;
}

// 比较内核：把 count 行数据的比较结果写入 SelectionMask。
//
// 约定：
//   - 内核负责先 result.SetNone(count)，再只写 [0, count) 的位。
//   - count <= kVectorBatchSize。
//   - 尾部不足一个向量的部分由内核自行标量处理。
//   - 语义为类型化比较（int64 不经过 double），避免 > 2^53 的精度损失。

// column cmp constant
using CompareI32ConstFn = void (*)(const int32_t* data, uint32_t count, CmpOp op, int32_t rhs, SelectionMask& result);
using CompareI64ConstFn = void (*)(const int64_t* data, uint32_t count, CmpOp op, int64_t rhs, SelectionMask& result);
using CompareF32ConstFn = void (*)(const float* data, uint32_t count, CmpOp op, float rhs, SelectionMask& result);
using CompareF64ConstFn = void (*)(const double* data, uint32_t count, CmpOp op, double rhs, SelectionMask& result);

// column cmp column
using CompareI32ColumnFn = void (*)(const int32_t* lhs, const int32_t* rhs, uint32_t count, CmpOp op,
                                    SelectionMask& result);
using CompareI64ColumnFn = void (*)(const int64_t* lhs, const int64_t* rhs, uint32_t count, CmpOp op,
                                    SelectionMask& result);
using CompareF32ColumnFn = void (*)(const float* lhs, const float* rhs, uint32_t count, CmpOp op,
                                    SelectionMask& result);
using CompareF64ColumnFn = void (*)(const double* lhs, const double* rhs, uint32_t count, CmpOp op,
                                    SelectionMask& result);

// 一组 compare 内核函数指针。scalar backend 与 avx2 backend 都填同一张表，
// 热路径只调用函数指针，不再执行 CPUID。
struct CompareKernels {
    CompareI32ConstFn i32_const = nullptr;
    CompareI64ConstFn i64_const = nullptr;
    CompareF32ConstFn f32_const = nullptr;
    CompareF64ConstFn f64_const = nullptr;

    CompareI32ColumnFn i32_column = nullptr;
    CompareI64ColumnFn i64_column = nullptr;
    CompareF32ColumnFn f32_column = nullptr;
    CompareF64ColumnFn f64_column = nullptr;
};

} // namespace simple_olap::simd
