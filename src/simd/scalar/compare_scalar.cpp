#include "../../type.h"
#include "../compare_kernel.h"
#include "../kernels.h"

namespace simple_olap::simd {
namespace {

template <typename T> void CompareConstScalar(const T* data, uint32_t count, CmpOp op, T rhs, SelectionMask& result) {
    result.SetNone(count);
    uint64_t* words = result.data();
    for (uint32_t i = 0; i < count; ++i) {
        if (CompareTypedValue<T>(data[i], op, rhs)) {
            words[i >> 6] |= (uint64_t(1) << (i & 63));
        }
    }
}

template <typename T>
void CompareColumnScalar(const T* lhs, const T* rhs, uint32_t count, CmpOp op, SelectionMask& result) {
    result.SetNone(count);
    uint64_t* words = result.data();
    for (uint32_t i = 0; i < count; ++i) {
        if (CompareTypedValue<T>(lhs[i], op, rhs[i])) {
            words[i >> 6] |= (uint64_t(1) << (i & 63));
        }
    }
}

void CompareI32Const(const int32_t* data, uint32_t count, CmpOp op, int32_t rhs, SelectionMask& result) {
    CompareConstScalar<int32_t>(data, count, op, rhs, result);
}
void CompareI64Const(const int64_t* data, uint32_t count, CmpOp op, int64_t rhs, SelectionMask& result) {
    CompareConstScalar<int64_t>(data, count, op, rhs, result);
}
void CompareF32Const(const float* data, uint32_t count, CmpOp op, float rhs, SelectionMask& result) {
    CompareConstScalar<float>(data, count, op, rhs, result);
}
void CompareF64Const(const double* data, uint32_t count, CmpOp op, double rhs, SelectionMask& result) {
    CompareConstScalar<double>(data, count, op, rhs, result);
}

void CompareI32Column(const int32_t* lhs, const int32_t* rhs, uint32_t count, CmpOp op, SelectionMask& result) {
    CompareColumnScalar<int32_t>(lhs, rhs, count, op, result);
}
void CompareI64Column(const int64_t* lhs, const int64_t* rhs, uint32_t count, CmpOp op, SelectionMask& result) {
    CompareColumnScalar<int64_t>(lhs, rhs, count, op, result);
}
void CompareF32Column(const float* lhs, const float* rhs, uint32_t count, CmpOp op, SelectionMask& result) {
    CompareColumnScalar<float>(lhs, rhs, count, op, result);
}
void CompareF64Column(const double* lhs, const double* rhs, uint32_t count, CmpOp op, SelectionMask& result) {
    CompareColumnScalar<double>(lhs, rhs, count, op, result);
}

} // namespace

void InstallScalarCompareKernels(CompareKernels& kernels) {
    kernels.i32_const = &CompareI32Const;
    kernels.i64_const = &CompareI64Const;
    kernels.f32_const = &CompareF32Const;
    kernels.f64_const = &CompareF64Const;

    kernels.i32_column = &CompareI32Column;
    kernels.i64_column = &CompareI64Column;
    kernels.f32_column = &CompareF32Column;
    kernels.f64_column = &CompareF64Column;
}

} // namespace simple_olap::simd
