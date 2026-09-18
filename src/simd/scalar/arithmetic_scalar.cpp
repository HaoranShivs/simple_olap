#include "../arithmetic_kernel.h"
#include "../kernels.h"

namespace simple_olap::simd {
namespace {

template <typename T> void ArithmeticColumnScalar(ArithmeticOp op, const T* lhs, const T* rhs, T* out, uint32_t count) {
    for (uint32_t i = 0; i < count; ++i) {
        out[i] = ArithmeticTypedValue<T>(lhs[i], op, rhs[i]);
    }
}

template <typename T>
void ArithmeticConstScalar(ArithmeticOp op, const T* data, T constant, bool const_on_left, T* out, uint32_t count) {
    for (uint32_t i = 0; i < count; ++i) {
        out[i] = const_on_left ? ArithmeticTypedValue<T>(constant, op, data[i])
                               : ArithmeticTypedValue<T>(data[i], op, constant);
    }
}

void ArithmeticI32Column(ArithmeticOp op, const int32_t* lhs, const int32_t* rhs, int32_t* out, uint32_t count) {
    ArithmeticColumnScalar<int32_t>(op, lhs, rhs, out, count);
}
void ArithmeticI64Column(ArithmeticOp op, const int64_t* lhs, const int64_t* rhs, int64_t* out, uint32_t count) {
    ArithmeticColumnScalar<int64_t>(op, lhs, rhs, out, count);
}
void ArithmeticF32Column(ArithmeticOp op, const float* lhs, const float* rhs, float* out, uint32_t count) {
    ArithmeticColumnScalar<float>(op, lhs, rhs, out, count);
}
void ArithmeticF64Column(ArithmeticOp op, const double* lhs, const double* rhs, double* out, uint32_t count) {
    ArithmeticColumnScalar<double>(op, lhs, rhs, out, count);
}

void ArithmeticI32Const(ArithmeticOp op, const int32_t* data, int32_t constant, bool const_on_left, int32_t* out,
                        uint32_t count) {
    ArithmeticConstScalar<int32_t>(op, data, constant, const_on_left, out, count);
}
void ArithmeticI64Const(ArithmeticOp op, const int64_t* data, int64_t constant, bool const_on_left, int64_t* out,
                        uint32_t count) {
    ArithmeticConstScalar<int64_t>(op, data, constant, const_on_left, out, count);
}
void ArithmeticF32Const(ArithmeticOp op, const float* data, float constant, bool const_on_left, float* out,
                        uint32_t count) {
    ArithmeticConstScalar<float>(op, data, constant, const_on_left, out, count);
}
void ArithmeticF64Const(ArithmeticOp op, const double* data, double constant, bool const_on_left, double* out,
                        uint32_t count) {
    ArithmeticConstScalar<double>(op, data, constant, const_on_left, out, count);
}

} // namespace

void InstallScalarArithmeticKernels(ArithmeticKernels& kernels) {
    kernels.i32_column = &ArithmeticI32Column;
    kernels.i64_column = &ArithmeticI64Column;
    kernels.f32_column = &ArithmeticF32Column;
    kernels.f64_column = &ArithmeticF64Column;

    kernels.i32_const = &ArithmeticI32Const;
    kernels.i64_const = &ArithmeticI64Const;
    kernels.f32_const = &ArithmeticF32Const;
    kernels.f64_const = &ArithmeticF64Const;
}

} // namespace simple_olap::simd
