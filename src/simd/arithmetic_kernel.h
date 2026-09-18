#pragma once

#include <cstdint>

namespace simple_olap::simd {

// ============================================================
// 算术内核：dense（无 selection）情况下的逐元素 ADD / SUB
// ============================================================
//
// 第一版刻意只做 ADD / SUB，不做乘除；也不处理 selection 非空的 gather
// 场景（由调用方退回标量物化）。
//
// 语义约定：
//   - 输入输出都是连续数组，out 允许与输入不重叠（当前调用方保证不重叠）。
//   - count 可以不是向量宽度的整数倍，内核自行处理尾部标量。
//   - 整数为定宽回绕运算；浮点为 IEEE 直接运算。

enum class ArithmeticOp : uint8_t {
    ADD = 0,
    SUB,
};

// 类型化标量语义：scalar backend 与 AVX2 backend 的尾部共用同一份实现，
// 保证两个 backend 对所有输入结果一致。
template <typename T> inline T ArithmeticTypedValue(T lhs, ArithmeticOp op, T rhs) {
    switch (op) {
    case ArithmeticOp::ADD:
        return lhs + rhs;
    case ArithmeticOp::SUB:
        return lhs - rhs;
    }
    return lhs;
}

// column OP column
using ArithmeticI32ColumnFn = void (*)(ArithmeticOp op, const int32_t* lhs, const int32_t* rhs, int32_t* out,
                                       uint32_t count);
using ArithmeticI64ColumnFn = void (*)(ArithmeticOp op, const int64_t* lhs, const int64_t* rhs, int64_t* out,
                                       uint32_t count);
using ArithmeticF32ColumnFn = void (*)(ArithmeticOp op, const float* lhs, const float* rhs, float* out, uint32_t count);
using ArithmeticF64ColumnFn = void (*)(ArithmeticOp op, const double* lhs, const double* rhs, double* out,
                                       uint32_t count);

// column OP constant 与 constant OP column 共用一个入口：
//   const_on_left == false -> out[i] = data[i] OP constant
//   const_on_left == true  -> out[i] = constant OP data[i]
using ArithmeticI32ConstFn = void (*)(ArithmeticOp op, const int32_t* data, int32_t constant, bool const_on_left,
                                      int32_t* out, uint32_t count);
using ArithmeticI64ConstFn = void (*)(ArithmeticOp op, const int64_t* data, int64_t constant, bool const_on_left,
                                      int64_t* out, uint32_t count);
using ArithmeticF32ConstFn = void (*)(ArithmeticOp op, const float* data, float constant, bool const_on_left,
                                      float* out, uint32_t count);
using ArithmeticF64ConstFn = void (*)(ArithmeticOp op, const double* data, double constant, bool const_on_left,
                                      double* out, uint32_t count);

// 一组算术内核函数指针；scalar backend 与 avx2 backend 都填同一张表。
struct ArithmeticKernels {
    ArithmeticI32ColumnFn i32_column = nullptr;
    ArithmeticI64ColumnFn i64_column = nullptr;
    ArithmeticF32ColumnFn f32_column = nullptr;
    ArithmeticF64ColumnFn f64_column = nullptr;

    ArithmeticI32ConstFn i32_const = nullptr;
    ArithmeticI64ConstFn i64_const = nullptr;
    ArithmeticF32ConstFn f32_const = nullptr;
    ArithmeticF64ConstFn f64_const = nullptr;
};

} // namespace simple_olap::simd
