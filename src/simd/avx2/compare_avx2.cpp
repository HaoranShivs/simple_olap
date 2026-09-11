#include "../compare_kernel.h"
#include "../kernels.h"

// 本 translation unit 在 CMake 中被单独施加 -mavx2；非 x86 平台不施加该选项，
// 且下面的实现体整体被条件编译屏蔽，只保留空的 InstallAvx2CompareKernels。

#if defined(__x86_64__) || defined(__i386__) || defined(_M_X64) || defined(_M_IX86)

#include <immintrin.h>

namespace simple_olap::simd {
namespace {

// ---------- 32-bit 比较（int32 / float，8 lanes）----------

inline __m256i CmpI32(__m256i a, __m256i b, CmpOp op) {
    switch (op) {
    case CmpOp::EQ:
        return _mm256_cmpeq_epi32(a, b);
    case CmpOp::NE:
        return _mm256_xor_si256(_mm256_cmpeq_epi32(a, b), _mm256_set1_epi32(-1));
    case CmpOp::GT:
        return _mm256_cmpgt_epi32(a, b);
    case CmpOp::LT:
        return _mm256_cmpgt_epi32(b, a);
    case CmpOp::GE:
        return _mm256_xor_si256(_mm256_cmpgt_epi32(b, a), _mm256_set1_epi32(-1));
    case CmpOp::LE:
        return _mm256_xor_si256(_mm256_cmpgt_epi32(a, b), _mm256_set1_epi32(-1));
    }
    return _mm256_setzero_si256();
}

inline void StoreBitsI32(uint64_t* words, uint32_t base, __m256i cmp) {
    const int mask = _mm256_movemask_ps(_mm256_castsi256_ps(cmp));
    words[base >> 6] |= (uint64_t(static_cast<uint32_t>(mask)) << (base & 63));
}

// ---------- 64-bit 比较（int64 / double，4 lanes）----------

inline __m256i CmpI64(__m256i a, __m256i b, CmpOp op) {
    switch (op) {
    case CmpOp::EQ:
        return _mm256_cmpeq_epi64(a, b);
    case CmpOp::NE:
        return _mm256_xor_si256(_mm256_cmpeq_epi64(a, b), _mm256_set1_epi64x(-1));
    case CmpOp::GT:
        return _mm256_cmpgt_epi64(a, b);
    case CmpOp::LT:
        return _mm256_cmpgt_epi64(b, a);
    case CmpOp::GE:
        return _mm256_xor_si256(_mm256_cmpgt_epi64(b, a), _mm256_set1_epi64x(-1));
    case CmpOp::LE:
        return _mm256_xor_si256(_mm256_cmpgt_epi64(a, b), _mm256_set1_epi64x(-1));
    }
    return _mm256_setzero_si256();
}

inline void StoreBitsI64(uint64_t* words, uint32_t base, __m256i cmp) {
    const int mask = _mm256_movemask_pd(_mm256_castsi256_pd(cmp));
    words[base >> 6] |= (uint64_t(static_cast<uint32_t>(mask)) << (base & 63));
}

// ---------- float（8 lanes）----------

inline __m256 CmpF32(__m256 a, __m256 b, CmpOp op) {
    switch (op) {
    case CmpOp::EQ:
        return _mm256_cmp_ps(a, b, _CMP_EQ_OQ);
    case CmpOp::NE:
        return _mm256_cmp_ps(a, b, _CMP_NEQ_UQ);
    case CmpOp::GT:
        return _mm256_cmp_ps(a, b, _CMP_GT_OQ);
    case CmpOp::GE:
        return _mm256_cmp_ps(a, b, _CMP_GE_OQ);
    case CmpOp::LT:
        return _mm256_cmp_ps(a, b, _CMP_LT_OQ);
    case CmpOp::LE:
        return _mm256_cmp_ps(a, b, _CMP_LE_OQ);
    }
    return _mm256_setzero_ps();
}

inline void StoreBitsF32(uint64_t* words, uint32_t base, __m256 cmp) {
    const int mask = _mm256_movemask_ps(cmp);
    words[base >> 6] |= (uint64_t(static_cast<uint32_t>(mask)) << (base & 63));
}

// ---------- double（4 lanes）----------

inline __m256d CmpF64(__m256d a, __m256d b, CmpOp op) {
    switch (op) {
    case CmpOp::EQ:
        return _mm256_cmp_pd(a, b, _CMP_EQ_OQ);
    case CmpOp::NE:
        return _mm256_cmp_pd(a, b, _CMP_NEQ_UQ);
    case CmpOp::GT:
        return _mm256_cmp_pd(a, b, _CMP_GT_OQ);
    case CmpOp::GE:
        return _mm256_cmp_pd(a, b, _CMP_GE_OQ);
    case CmpOp::LT:
        return _mm256_cmp_pd(a, b, _CMP_LT_OQ);
    case CmpOp::LE:
        return _mm256_cmp_pd(a, b, _CMP_LE_OQ);
    }
    return _mm256_setzero_pd();
}

inline void StoreBitsF64(uint64_t* words, uint32_t base, __m256d cmp) {
    const int mask = _mm256_movemask_pd(cmp);
    words[base >> 6] |= (uint64_t(static_cast<uint32_t>(mask)) << (base & 63));
}

// ---------- const compare kernels ----------

void CompareI32ConstAvx2(const int32_t* data, uint32_t count, CmpOp op, int32_t rhs, SelectionMask& result) {
    result.SetNone(count);
    uint64_t* words = result.data();
    const __m256i vrhs = _mm256_set1_epi32(rhs);

    uint32_t i = 0;
    for (; i + 8 <= count; i += 8) {
        const __m256i v = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(data + i));
        StoreBitsI32(words, i, CmpI32(v, vrhs, op));
    }
    for (; i < count; ++i) {
        if (CompareTypedValue(data[i], op, rhs)) {
            words[i >> 6] |= (uint64_t(1) << (i & 63));
        }
    }
}

void CompareI64ConstAvx2(const int64_t* data, uint32_t count, CmpOp op, int64_t rhs, SelectionMask& result) {
    result.SetNone(count);
    uint64_t* words = result.data();
    const __m256i vrhs = _mm256_set1_epi64x(rhs);

    uint32_t i = 0;
    for (; i + 4 <= count; i += 4) {
        const __m256i v = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(data + i));
        StoreBitsI64(words, i, CmpI64(v, vrhs, op));
    }
    for (; i < count; ++i) {
        if (CompareTypedValue(data[i], op, rhs)) {
            words[i >> 6] |= (uint64_t(1) << (i & 63));
        }
    }
}

void CompareF32ConstAvx2(const float* data, uint32_t count, CmpOp op, float rhs, SelectionMask& result) {
    result.SetNone(count);
    uint64_t* words = result.data();
    const __m256 vrhs = _mm256_set1_ps(rhs);

    uint32_t i = 0;
    for (; i + 8 <= count; i += 8) {
        const __m256 v = _mm256_loadu_ps(data + i);
        StoreBitsF32(words, i, CmpF32(v, vrhs, op));
    }
    for (; i < count; ++i) {
        if (CompareTypedValue(data[i], op, rhs)) {
            words[i >> 6] |= (uint64_t(1) << (i & 63));
        }
    }
}

void CompareF64ConstAvx2(const double* data, uint32_t count, CmpOp op, double rhs, SelectionMask& result) {
    result.SetNone(count);
    uint64_t* words = result.data();
    const __m256d vrhs = _mm256_set1_pd(rhs);

    uint32_t i = 0;
    for (; i + 4 <= count; i += 4) {
        const __m256d v = _mm256_loadu_pd(data + i);
        StoreBitsF64(words, i, CmpF64(v, vrhs, op));
    }
    for (; i < count; ++i) {
        if (CompareTypedValue(data[i], op, rhs)) {
            words[i >> 6] |= (uint64_t(1) << (i & 63));
        }
    }
}

// ---------- column compare kernels ----------

void CompareI32ColumnAvx2(const int32_t* lhs, const int32_t* rhs, uint32_t count, CmpOp op, SelectionMask& result) {
    result.SetNone(count);
    uint64_t* words = result.data();

    uint32_t i = 0;
    for (; i + 8 <= count; i += 8) {
        const __m256i a = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(lhs + i));
        const __m256i b = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(rhs + i));
        StoreBitsI32(words, i, CmpI32(a, b, op));
    }
    for (; i < count; ++i) {
        if (CompareTypedValue(lhs[i], op, rhs[i])) {
            words[i >> 6] |= (uint64_t(1) << (i & 63));
        }
    }
}

void CompareI64ColumnAvx2(const int64_t* lhs, const int64_t* rhs, uint32_t count, CmpOp op, SelectionMask& result) {
    result.SetNone(count);
    uint64_t* words = result.data();

    uint32_t i = 0;
    for (; i + 4 <= count; i += 4) {
        const __m256i a = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(lhs + i));
        const __m256i b = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(rhs + i));
        StoreBitsI64(words, i, CmpI64(a, b, op));
    }
    for (; i < count; ++i) {
        if (CompareTypedValue(lhs[i], op, rhs[i])) {
            words[i >> 6] |= (uint64_t(1) << (i & 63));
        }
    }
}

void CompareF32ColumnAvx2(const float* lhs, const float* rhs, uint32_t count, CmpOp op, SelectionMask& result) {
    result.SetNone(count);
    uint64_t* words = result.data();

    uint32_t i = 0;
    for (; i + 8 <= count; i += 8) {
        const __m256 a = _mm256_loadu_ps(lhs + i);
        const __m256 b = _mm256_loadu_ps(rhs + i);
        StoreBitsF32(words, i, CmpF32(a, b, op));
    }
    for (; i < count; ++i) {
        if (CompareTypedValue(lhs[i], op, rhs[i])) {
            words[i >> 6] |= (uint64_t(1) << (i & 63));
        }
    }
}

void CompareF64ColumnAvx2(const double* lhs, const double* rhs, uint32_t count, CmpOp op, SelectionMask& result) {
    result.SetNone(count);
    uint64_t* words = result.data();

    uint32_t i = 0;
    for (; i + 4 <= count; i += 4) {
        const __m256d a = _mm256_loadu_pd(lhs + i);
        const __m256d b = _mm256_loadu_pd(rhs + i);
        StoreBitsF64(words, i, CmpF64(a, b, op));
    }
    for (; i < count; ++i) {
        if (CompareTypedValue(lhs[i], op, rhs[i])) {
            words[i >> 6] |= (uint64_t(1) << (i & 63));
        }
    }
}

} // namespace
} // namespace simple_olap::simd

#endif // x86

namespace simple_olap::simd {

void InstallAvx2CompareKernels(CompareKernels& kernels) {
#if defined(__x86_64__) || defined(__i386__) || defined(_M_X64) || defined(_M_IX86)
    kernels.i32_const = &CompareI32ConstAvx2;
    kernels.i64_const = &CompareI64ConstAvx2;
    kernels.f32_const = &CompareF32ConstAvx2;
    kernels.f64_const = &CompareF64ConstAvx2;

    kernels.i32_column = &CompareI32ColumnAvx2;
    kernels.i64_column = &CompareI64ColumnAvx2;
    kernels.f32_column = &CompareF32ColumnAvx2;
    kernels.f64_column = &CompareF64ColumnAvx2;
#else
    (void)kernels;
#endif
}

} // namespace simple_olap::simd
