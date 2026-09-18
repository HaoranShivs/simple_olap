#include "../arithmetic_kernel.h"
#include "../kernels.h"

// 本 translation unit 在 CMake 中被单独施加 -mavx2；非 x86 平台不施加该选项，
// 且下面的实现体整体被条件编译屏蔽，只保留空的 InstallAvx2ArithmeticKernels。

#if defined(__x86_64__) || defined(__i386__) || defined(_M_X64) || defined(_M_IX86)

#include <immintrin.h>

namespace simple_olap::simd {
namespace {

// ---------- column OP column ----------

void ArithmeticI32ColumnAvx2(ArithmeticOp op, const int32_t* lhs, const int32_t* rhs, int32_t* out, uint32_t count) {
    uint32_t i = 0;
    for (; i + 8 <= count; i += 8) {
        const __m256i a = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(lhs + i));
        const __m256i b = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(rhs + i));
        const __m256i r = (op == ArithmeticOp::ADD) ? _mm256_add_epi32(a, b) : _mm256_sub_epi32(a, b);
        _mm256_storeu_si256(reinterpret_cast<__m256i*>(out + i), r);
    }
    for (; i < count; ++i) {
        out[i] = ArithmeticTypedValue<int32_t>(lhs[i], op, rhs[i]);
    }
}

void ArithmeticI64ColumnAvx2(ArithmeticOp op, const int64_t* lhs, const int64_t* rhs, int64_t* out, uint32_t count) {
    uint32_t i = 0;
    for (; i + 4 <= count; i += 4) {
        const __m256i a = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(lhs + i));
        const __m256i b = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(rhs + i));
        const __m256i r = (op == ArithmeticOp::ADD) ? _mm256_add_epi64(a, b) : _mm256_sub_epi64(a, b);
        _mm256_storeu_si256(reinterpret_cast<__m256i*>(out + i), r);
    }
    for (; i < count; ++i) {
        out[i] = ArithmeticTypedValue<int64_t>(lhs[i], op, rhs[i]);
    }
}

void ArithmeticF32ColumnAvx2(ArithmeticOp op, const float* lhs, const float* rhs, float* out, uint32_t count) {
    uint32_t i = 0;
    for (; i + 8 <= count; i += 8) {
        const __m256 a = _mm256_loadu_ps(lhs + i);
        const __m256 b = _mm256_loadu_ps(rhs + i);
        const __m256 r = (op == ArithmeticOp::ADD) ? _mm256_add_ps(a, b) : _mm256_sub_ps(a, b);
        _mm256_storeu_ps(out + i, r);
    }
    for (; i < count; ++i) {
        out[i] = ArithmeticTypedValue<float>(lhs[i], op, rhs[i]);
    }
}

void ArithmeticF64ColumnAvx2(ArithmeticOp op, const double* lhs, const double* rhs, double* out, uint32_t count) {
    uint32_t i = 0;
    for (; i + 4 <= count; i += 4) {
        const __m256d a = _mm256_loadu_pd(lhs + i);
        const __m256d b = _mm256_loadu_pd(rhs + i);
        const __m256d r = (op == ArithmeticOp::ADD) ? _mm256_add_pd(a, b) : _mm256_sub_pd(a, b);
        _mm256_storeu_pd(out + i, r);
    }
    for (; i < count; ++i) {
        out[i] = ArithmeticTypedValue<double>(lhs[i], op, rhs[i]);
    }
}

// ---------- column OP constant / constant OP column ----------
//
// 只有 SUB 需要区分常量在哪一侧；ADD 可交换。

void ArithmeticI32ConstAvx2(ArithmeticOp op, const int32_t* data, int32_t constant, bool const_on_left, int32_t* out,
                            uint32_t count) {
    const __m256i vc = _mm256_set1_epi32(constant);
    uint32_t i = 0;
    for (; i + 8 <= count; i += 8) {
        const __m256i v = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(data + i));
        __m256i r;
        if (op == ArithmeticOp::ADD) {
            r = _mm256_add_epi32(v, vc);
        } else {
            r = const_on_left ? _mm256_sub_epi32(vc, v) : _mm256_sub_epi32(v, vc);
        }
        _mm256_storeu_si256(reinterpret_cast<__m256i*>(out + i), r);
    }
    for (; i < count; ++i) {
        out[i] = const_on_left ? ArithmeticTypedValue<int32_t>(constant, op, data[i])
                               : ArithmeticTypedValue<int32_t>(data[i], op, constant);
    }
}

void ArithmeticI64ConstAvx2(ArithmeticOp op, const int64_t* data, int64_t constant, bool const_on_left, int64_t* out,
                            uint32_t count) {
    const __m256i vc = _mm256_set1_epi64x(constant);
    uint32_t i = 0;
    for (; i + 4 <= count; i += 4) {
        const __m256i v = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(data + i));
        __m256i r;
        if (op == ArithmeticOp::ADD) {
            r = _mm256_add_epi64(v, vc);
        } else {
            r = const_on_left ? _mm256_sub_epi64(vc, v) : _mm256_sub_epi64(v, vc);
        }
        _mm256_storeu_si256(reinterpret_cast<__m256i*>(out + i), r);
    }
    for (; i < count; ++i) {
        out[i] = const_on_left ? ArithmeticTypedValue<int64_t>(constant, op, data[i])
                               : ArithmeticTypedValue<int64_t>(data[i], op, constant);
    }
}

void ArithmeticF32ConstAvx2(ArithmeticOp op, const float* data, float constant, bool const_on_left, float* out,
                            uint32_t count) {
    const __m256 vc = _mm256_set1_ps(constant);
    uint32_t i = 0;
    for (; i + 8 <= count; i += 8) {
        const __m256 v = _mm256_loadu_ps(data + i);
        __m256 r;
        if (op == ArithmeticOp::ADD) {
            r = _mm256_add_ps(v, vc);
        } else {
            r = const_on_left ? _mm256_sub_ps(vc, v) : _mm256_sub_ps(v, vc);
        }
        _mm256_storeu_ps(out + i, r);
    }
    for (; i < count; ++i) {
        out[i] = const_on_left ? ArithmeticTypedValue<float>(constant, op, data[i])
                               : ArithmeticTypedValue<float>(data[i], op, constant);
    }
}

void ArithmeticF64ConstAvx2(ArithmeticOp op, const double* data, double constant, bool const_on_left, double* out,
                            uint32_t count) {
    const __m256d vc = _mm256_set1_pd(constant);
    uint32_t i = 0;
    for (; i + 4 <= count; i += 4) {
        const __m256d v = _mm256_loadu_pd(data + i);
        __m256d r;
        if (op == ArithmeticOp::ADD) {
            r = _mm256_add_pd(v, vc);
        } else {
            r = const_on_left ? _mm256_sub_pd(vc, v) : _mm256_sub_pd(v, vc);
        }
        _mm256_storeu_pd(out + i, r);
    }
    for (; i < count; ++i) {
        out[i] = const_on_left ? ArithmeticTypedValue<double>(constant, op, data[i])
                               : ArithmeticTypedValue<double>(data[i], op, constant);
    }
}

} // namespace
} // namespace simple_olap::simd

#endif // x86

namespace simple_olap::simd {

void InstallAvx2ArithmeticKernels(ArithmeticKernels& kernels) {
#if defined(__x86_64__) || defined(__i386__) || defined(_M_X64) || defined(_M_IX86)
    kernels.i32_column = &ArithmeticI32ColumnAvx2;
    kernels.i64_column = &ArithmeticI64ColumnAvx2;
    kernels.f32_column = &ArithmeticF32ColumnAvx2;
    kernels.f64_column = &ArithmeticF64ColumnAvx2;

    kernels.i32_const = &ArithmeticI32ConstAvx2;
    kernels.i64_const = &ArithmeticI64ConstAvx2;
    kernels.f32_const = &ArithmeticF32ConstAvx2;
    kernels.f64_const = &ArithmeticF64ConstAvx2;
#else
    (void)kernels;
#endif
}

} // namespace simple_olap::simd
