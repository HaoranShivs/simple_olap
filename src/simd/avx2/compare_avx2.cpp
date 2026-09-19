#include "../compare_kernel.h"
#include "../kernels.h"

// 本 translation unit 在 CMake 中被单独施加 -mavx2；非 x86 平台不施加该选项，
// 且下面的实现体整体被条件编译屏蔽，只保留空的 InstallAvx2CompareKernels。

#if defined(__x86_64__) || defined(__i386__) || defined(_M_X64) || defined(_M_IX86)

#include <immintrin.h>

namespace simple_olap::simd {
namespace {

// ============================================================
// AVX2 compare 内核：三级循环
// ============================================================
//
//   1. 64-row block：8 个 AVX2 向量在寄存器中拼出一个完整 uint64_t bitmap，
//      每个 full word 只写一次（words[word] = block），没有 read-modify-write。
//   2. AVX2 remainder：不足 64 行的部分按 8/4 行处理，沿用原有 |= 写法。
//   3. scalar tail：不足一个向量的尾部逐行比较。
//
// 64-row 拼装故意使用多条独立累加链（32-bit 用 low/high 两条，
// 64-bit 用 q0..q3 四条），避免单条 8/16 层 OR dependency chain。
//
// 对外接口与 KernelRegistry 完全不变：仍然只填充 CompareKernels 函数表。

// ---------- movemask helper ----------

inline uint32_t MaskI32(__m256i cmp) {
    return static_cast<uint32_t>(_mm256_movemask_ps(_mm256_castsi256_ps(cmp)));
}

inline uint32_t MaskI64(__m256i cmp) {
    return static_cast<uint32_t>(_mm256_movemask_pd(_mm256_castsi256_pd(cmp)));
}

inline uint32_t MaskF32(__m256 cmp) {
    return static_cast<uint32_t>(_mm256_movemask_ps(cmp));
}

inline uint32_t MaskF64(__m256d cmp) {
    return static_cast<uint32_t>(_mm256_movemask_pd(cmp));
}

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
    words[base >> 6] |= (uint64_t(MaskI32(cmp)) << (base & 63));
}

// 64 行（8 个 AVX2 向量）-> 一个完整 bitmap word。
inline uint64_t Compare64I32Const(const int32_t* data, __m256i rhs, CmpOp op) {
    const uint64_t m0 = MaskI32(CmpI32(_mm256_loadu_si256(reinterpret_cast<const __m256i*>(data + 0)), rhs, op));
    const uint64_t m1 = MaskI32(CmpI32(_mm256_loadu_si256(reinterpret_cast<const __m256i*>(data + 8)), rhs, op));
    const uint64_t m2 = MaskI32(CmpI32(_mm256_loadu_si256(reinterpret_cast<const __m256i*>(data + 16)), rhs, op));
    const uint64_t m3 = MaskI32(CmpI32(_mm256_loadu_si256(reinterpret_cast<const __m256i*>(data + 24)), rhs, op));
    const uint64_t m4 = MaskI32(CmpI32(_mm256_loadu_si256(reinterpret_cast<const __m256i*>(data + 32)), rhs, op));
    const uint64_t m5 = MaskI32(CmpI32(_mm256_loadu_si256(reinterpret_cast<const __m256i*>(data + 40)), rhs, op));
    const uint64_t m6 = MaskI32(CmpI32(_mm256_loadu_si256(reinterpret_cast<const __m256i*>(data + 48)), rhs, op));
    const uint64_t m7 = MaskI32(CmpI32(_mm256_loadu_si256(reinterpret_cast<const __m256i*>(data + 56)), rhs, op));

    const uint64_t low = m0 | (m1 << 8) | (m2 << 16) | (m3 << 24);
    const uint64_t high = m4 | (m5 << 8) | (m6 << 16) | (m7 << 24);
    return low | (high << 32);
}

inline uint64_t Compare64I32Column(const int32_t* lhs, const int32_t* rhs, CmpOp op) {
    const uint64_t m0 = MaskI32(CmpI32(_mm256_loadu_si256(reinterpret_cast<const __m256i*>(lhs + 0)),
                                       _mm256_loadu_si256(reinterpret_cast<const __m256i*>(rhs + 0)), op));
    const uint64_t m1 = MaskI32(CmpI32(_mm256_loadu_si256(reinterpret_cast<const __m256i*>(lhs + 8)),
                                       _mm256_loadu_si256(reinterpret_cast<const __m256i*>(rhs + 8)), op));
    const uint64_t m2 = MaskI32(CmpI32(_mm256_loadu_si256(reinterpret_cast<const __m256i*>(lhs + 16)),
                                       _mm256_loadu_si256(reinterpret_cast<const __m256i*>(rhs + 16)), op));
    const uint64_t m3 = MaskI32(CmpI32(_mm256_loadu_si256(reinterpret_cast<const __m256i*>(lhs + 24)),
                                       _mm256_loadu_si256(reinterpret_cast<const __m256i*>(rhs + 24)), op));
    const uint64_t m4 = MaskI32(CmpI32(_mm256_loadu_si256(reinterpret_cast<const __m256i*>(lhs + 32)),
                                       _mm256_loadu_si256(reinterpret_cast<const __m256i*>(rhs + 32)), op));
    const uint64_t m5 = MaskI32(CmpI32(_mm256_loadu_si256(reinterpret_cast<const __m256i*>(lhs + 40)),
                                       _mm256_loadu_si256(reinterpret_cast<const __m256i*>(rhs + 40)), op));
    const uint64_t m6 = MaskI32(CmpI32(_mm256_loadu_si256(reinterpret_cast<const __m256i*>(lhs + 48)),
                                       _mm256_loadu_si256(reinterpret_cast<const __m256i*>(rhs + 48)), op));
    const uint64_t m7 = MaskI32(CmpI32(_mm256_loadu_si256(reinterpret_cast<const __m256i*>(lhs + 56)),
                                       _mm256_loadu_si256(reinterpret_cast<const __m256i*>(rhs + 56)), op));

    const uint64_t low = m0 | (m1 << 8) | (m2 << 16) | (m3 << 24);
    const uint64_t high = m4 | (m5 << 8) | (m6 << 16) | (m7 << 24);
    return low | (high << 32);
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
    words[base >> 6] |= (uint64_t(MaskF32(cmp)) << (base & 63));
}

inline uint64_t Compare64F32Const(const float* data, __m256 rhs, CmpOp op) {
    const uint64_t m0 = MaskF32(CmpF32(_mm256_loadu_ps(data + 0), rhs, op));
    const uint64_t m1 = MaskF32(CmpF32(_mm256_loadu_ps(data + 8), rhs, op));
    const uint64_t m2 = MaskF32(CmpF32(_mm256_loadu_ps(data + 16), rhs, op));
    const uint64_t m3 = MaskF32(CmpF32(_mm256_loadu_ps(data + 24), rhs, op));
    const uint64_t m4 = MaskF32(CmpF32(_mm256_loadu_ps(data + 32), rhs, op));
    const uint64_t m5 = MaskF32(CmpF32(_mm256_loadu_ps(data + 40), rhs, op));
    const uint64_t m6 = MaskF32(CmpF32(_mm256_loadu_ps(data + 48), rhs, op));
    const uint64_t m7 = MaskF32(CmpF32(_mm256_loadu_ps(data + 56), rhs, op));

    const uint64_t low = m0 | (m1 << 8) | (m2 << 16) | (m3 << 24);
    const uint64_t high = m4 | (m5 << 8) | (m6 << 16) | (m7 << 24);
    return low | (high << 32);
}

inline uint64_t Compare64F32Column(const float* lhs, const float* rhs, CmpOp op) {
    const uint64_t m0 = MaskF32(CmpF32(_mm256_loadu_ps(lhs + 0), _mm256_loadu_ps(rhs + 0), op));
    const uint64_t m1 = MaskF32(CmpF32(_mm256_loadu_ps(lhs + 8), _mm256_loadu_ps(rhs + 8), op));
    const uint64_t m2 = MaskF32(CmpF32(_mm256_loadu_ps(lhs + 16), _mm256_loadu_ps(rhs + 16), op));
    const uint64_t m3 = MaskF32(CmpF32(_mm256_loadu_ps(lhs + 24), _mm256_loadu_ps(rhs + 24), op));
    const uint64_t m4 = MaskF32(CmpF32(_mm256_loadu_ps(lhs + 32), _mm256_loadu_ps(rhs + 32), op));
    const uint64_t m5 = MaskF32(CmpF32(_mm256_loadu_ps(lhs + 40), _mm256_loadu_ps(rhs + 40), op));
    const uint64_t m6 = MaskF32(CmpF32(_mm256_loadu_ps(lhs + 48), _mm256_loadu_ps(rhs + 48), op));
    const uint64_t m7 = MaskF32(CmpF32(_mm256_loadu_ps(lhs + 56), _mm256_loadu_ps(rhs + 56), op));

    const uint64_t low = m0 | (m1 << 8) | (m2 << 16) | (m3 << 24);
    const uint64_t high = m4 | (m5 << 8) | (m6 << 16) | (m7 << 24);
    return low | (high << 32);
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
    words[base >> 6] |= (uint64_t(MaskI64(cmp)) << (base & 63));
}

// 64 行（16 个 AVX2 向量）-> 一个完整 bitmap word；4 条独立累加链。
inline uint64_t Compare64I64Const(const int64_t* data, __m256i rhs, CmpOp op) {
    const uint64_t m0 = MaskI64(CmpI64(_mm256_loadu_si256(reinterpret_cast<const __m256i*>(data + 0)), rhs, op));
    const uint64_t m1 = MaskI64(CmpI64(_mm256_loadu_si256(reinterpret_cast<const __m256i*>(data + 4)), rhs, op));
    const uint64_t m2 = MaskI64(CmpI64(_mm256_loadu_si256(reinterpret_cast<const __m256i*>(data + 8)), rhs, op));
    const uint64_t m3 = MaskI64(CmpI64(_mm256_loadu_si256(reinterpret_cast<const __m256i*>(data + 12)), rhs, op));
    const uint64_t m4 = MaskI64(CmpI64(_mm256_loadu_si256(reinterpret_cast<const __m256i*>(data + 16)), rhs, op));
    const uint64_t m5 = MaskI64(CmpI64(_mm256_loadu_si256(reinterpret_cast<const __m256i*>(data + 20)), rhs, op));
    const uint64_t m6 = MaskI64(CmpI64(_mm256_loadu_si256(reinterpret_cast<const __m256i*>(data + 24)), rhs, op));
    const uint64_t m7 = MaskI64(CmpI64(_mm256_loadu_si256(reinterpret_cast<const __m256i*>(data + 28)), rhs, op));
    const uint64_t m8 = MaskI64(CmpI64(_mm256_loadu_si256(reinterpret_cast<const __m256i*>(data + 32)), rhs, op));
    const uint64_t m9 = MaskI64(CmpI64(_mm256_loadu_si256(reinterpret_cast<const __m256i*>(data + 36)), rhs, op));
    const uint64_t m10 = MaskI64(CmpI64(_mm256_loadu_si256(reinterpret_cast<const __m256i*>(data + 40)), rhs, op));
    const uint64_t m11 = MaskI64(CmpI64(_mm256_loadu_si256(reinterpret_cast<const __m256i*>(data + 44)), rhs, op));
    const uint64_t m12 = MaskI64(CmpI64(_mm256_loadu_si256(reinterpret_cast<const __m256i*>(data + 48)), rhs, op));
    const uint64_t m13 = MaskI64(CmpI64(_mm256_loadu_si256(reinterpret_cast<const __m256i*>(data + 52)), rhs, op));
    const uint64_t m14 = MaskI64(CmpI64(_mm256_loadu_si256(reinterpret_cast<const __m256i*>(data + 56)), rhs, op));
    const uint64_t m15 = MaskI64(CmpI64(_mm256_loadu_si256(reinterpret_cast<const __m256i*>(data + 60)), rhs, op));

    const uint64_t q0 = m0 | (m1 << 4) | (m2 << 8) | (m3 << 12);
    const uint64_t q1 = m4 | (m5 << 4) | (m6 << 8) | (m7 << 12);
    const uint64_t q2 = m8 | (m9 << 4) | (m10 << 8) | (m11 << 12);
    const uint64_t q3 = m12 | (m13 << 4) | (m14 << 8) | (m15 << 12);
    return q0 | (q1 << 16) | (q2 << 32) | (q3 << 48);
}

inline uint64_t Compare64I64Column(const int64_t* lhs, const int64_t* rhs, CmpOp op) {
    const uint64_t m0 = MaskI64(CmpI64(_mm256_loadu_si256(reinterpret_cast<const __m256i*>(lhs + 0)),
                                       _mm256_loadu_si256(reinterpret_cast<const __m256i*>(rhs + 0)), op));
    const uint64_t m1 = MaskI64(CmpI64(_mm256_loadu_si256(reinterpret_cast<const __m256i*>(lhs + 4)),
                                       _mm256_loadu_si256(reinterpret_cast<const __m256i*>(rhs + 4)), op));
    const uint64_t m2 = MaskI64(CmpI64(_mm256_loadu_si256(reinterpret_cast<const __m256i*>(lhs + 8)),
                                       _mm256_loadu_si256(reinterpret_cast<const __m256i*>(rhs + 8)), op));
    const uint64_t m3 = MaskI64(CmpI64(_mm256_loadu_si256(reinterpret_cast<const __m256i*>(lhs + 12)),
                                       _mm256_loadu_si256(reinterpret_cast<const __m256i*>(rhs + 12)), op));
    const uint64_t m4 = MaskI64(CmpI64(_mm256_loadu_si256(reinterpret_cast<const __m256i*>(lhs + 16)),
                                       _mm256_loadu_si256(reinterpret_cast<const __m256i*>(rhs + 16)), op));
    const uint64_t m5 = MaskI64(CmpI64(_mm256_loadu_si256(reinterpret_cast<const __m256i*>(lhs + 20)),
                                       _mm256_loadu_si256(reinterpret_cast<const __m256i*>(rhs + 20)), op));
    const uint64_t m6 = MaskI64(CmpI64(_mm256_loadu_si256(reinterpret_cast<const __m256i*>(lhs + 24)),
                                       _mm256_loadu_si256(reinterpret_cast<const __m256i*>(rhs + 24)), op));
    const uint64_t m7 = MaskI64(CmpI64(_mm256_loadu_si256(reinterpret_cast<const __m256i*>(lhs + 28)),
                                       _mm256_loadu_si256(reinterpret_cast<const __m256i*>(rhs + 28)), op));
    const uint64_t m8 = MaskI64(CmpI64(_mm256_loadu_si256(reinterpret_cast<const __m256i*>(lhs + 32)),
                                       _mm256_loadu_si256(reinterpret_cast<const __m256i*>(rhs + 32)), op));
    const uint64_t m9 = MaskI64(CmpI64(_mm256_loadu_si256(reinterpret_cast<const __m256i*>(lhs + 36)),
                                       _mm256_loadu_si256(reinterpret_cast<const __m256i*>(rhs + 36)), op));
    const uint64_t m10 = MaskI64(CmpI64(_mm256_loadu_si256(reinterpret_cast<const __m256i*>(lhs + 40)),
                                        _mm256_loadu_si256(reinterpret_cast<const __m256i*>(rhs + 40)), op));
    const uint64_t m11 = MaskI64(CmpI64(_mm256_loadu_si256(reinterpret_cast<const __m256i*>(lhs + 44)),
                                        _mm256_loadu_si256(reinterpret_cast<const __m256i*>(rhs + 44)), op));
    const uint64_t m12 = MaskI64(CmpI64(_mm256_loadu_si256(reinterpret_cast<const __m256i*>(lhs + 48)),
                                        _mm256_loadu_si256(reinterpret_cast<const __m256i*>(rhs + 48)), op));
    const uint64_t m13 = MaskI64(CmpI64(_mm256_loadu_si256(reinterpret_cast<const __m256i*>(lhs + 52)),
                                        _mm256_loadu_si256(reinterpret_cast<const __m256i*>(rhs + 52)), op));
    const uint64_t m14 = MaskI64(CmpI64(_mm256_loadu_si256(reinterpret_cast<const __m256i*>(lhs + 56)),
                                        _mm256_loadu_si256(reinterpret_cast<const __m256i*>(rhs + 56)), op));
    const uint64_t m15 = MaskI64(CmpI64(_mm256_loadu_si256(reinterpret_cast<const __m256i*>(lhs + 60)),
                                        _mm256_loadu_si256(reinterpret_cast<const __m256i*>(rhs + 60)), op));

    const uint64_t q0 = m0 | (m1 << 4) | (m2 << 8) | (m3 << 12);
    const uint64_t q1 = m4 | (m5 << 4) | (m6 << 8) | (m7 << 12);
    const uint64_t q2 = m8 | (m9 << 4) | (m10 << 8) | (m11 << 12);
    const uint64_t q3 = m12 | (m13 << 4) | (m14 << 8) | (m15 << 12);
    return q0 | (q1 << 16) | (q2 << 32) | (q3 << 48);
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
    words[base >> 6] |= (uint64_t(MaskF64(cmp)) << (base & 63));
}

inline uint64_t Compare64F64Const(const double* data, __m256d rhs, CmpOp op) {
    const uint64_t m0 = MaskF64(CmpF64(_mm256_loadu_pd(data + 0), rhs, op));
    const uint64_t m1 = MaskF64(CmpF64(_mm256_loadu_pd(data + 4), rhs, op));
    const uint64_t m2 = MaskF64(CmpF64(_mm256_loadu_pd(data + 8), rhs, op));
    const uint64_t m3 = MaskF64(CmpF64(_mm256_loadu_pd(data + 12), rhs, op));
    const uint64_t m4 = MaskF64(CmpF64(_mm256_loadu_pd(data + 16), rhs, op));
    const uint64_t m5 = MaskF64(CmpF64(_mm256_loadu_pd(data + 20), rhs, op));
    const uint64_t m6 = MaskF64(CmpF64(_mm256_loadu_pd(data + 24), rhs, op));
    const uint64_t m7 = MaskF64(CmpF64(_mm256_loadu_pd(data + 28), rhs, op));
    const uint64_t m8 = MaskF64(CmpF64(_mm256_loadu_pd(data + 32), rhs, op));
    const uint64_t m9 = MaskF64(CmpF64(_mm256_loadu_pd(data + 36), rhs, op));
    const uint64_t m10 = MaskF64(CmpF64(_mm256_loadu_pd(data + 40), rhs, op));
    const uint64_t m11 = MaskF64(CmpF64(_mm256_loadu_pd(data + 44), rhs, op));
    const uint64_t m12 = MaskF64(CmpF64(_mm256_loadu_pd(data + 48), rhs, op));
    const uint64_t m13 = MaskF64(CmpF64(_mm256_loadu_pd(data + 52), rhs, op));
    const uint64_t m14 = MaskF64(CmpF64(_mm256_loadu_pd(data + 56), rhs, op));
    const uint64_t m15 = MaskF64(CmpF64(_mm256_loadu_pd(data + 60), rhs, op));

    const uint64_t q0 = m0 | (m1 << 4) | (m2 << 8) | (m3 << 12);
    const uint64_t q1 = m4 | (m5 << 4) | (m6 << 8) | (m7 << 12);
    const uint64_t q2 = m8 | (m9 << 4) | (m10 << 8) | (m11 << 12);
    const uint64_t q3 = m12 | (m13 << 4) | (m14 << 8) | (m15 << 12);
    return q0 | (q1 << 16) | (q2 << 32) | (q3 << 48);
}

inline uint64_t Compare64F64Column(const double* lhs, const double* rhs, CmpOp op) {
    const uint64_t m0 = MaskF64(CmpF64(_mm256_loadu_pd(lhs + 0), _mm256_loadu_pd(rhs + 0), op));
    const uint64_t m1 = MaskF64(CmpF64(_mm256_loadu_pd(lhs + 4), _mm256_loadu_pd(rhs + 4), op));
    const uint64_t m2 = MaskF64(CmpF64(_mm256_loadu_pd(lhs + 8), _mm256_loadu_pd(rhs + 8), op));
    const uint64_t m3 = MaskF64(CmpF64(_mm256_loadu_pd(lhs + 12), _mm256_loadu_pd(rhs + 12), op));
    const uint64_t m4 = MaskF64(CmpF64(_mm256_loadu_pd(lhs + 16), _mm256_loadu_pd(rhs + 16), op));
    const uint64_t m5 = MaskF64(CmpF64(_mm256_loadu_pd(lhs + 20), _mm256_loadu_pd(rhs + 20), op));
    const uint64_t m6 = MaskF64(CmpF64(_mm256_loadu_pd(lhs + 24), _mm256_loadu_pd(rhs + 24), op));
    const uint64_t m7 = MaskF64(CmpF64(_mm256_loadu_pd(lhs + 28), _mm256_loadu_pd(rhs + 28), op));
    const uint64_t m8 = MaskF64(CmpF64(_mm256_loadu_pd(lhs + 32), _mm256_loadu_pd(rhs + 32), op));
    const uint64_t m9 = MaskF64(CmpF64(_mm256_loadu_pd(lhs + 36), _mm256_loadu_pd(rhs + 36), op));
    const uint64_t m10 = MaskF64(CmpF64(_mm256_loadu_pd(lhs + 40), _mm256_loadu_pd(rhs + 40), op));
    const uint64_t m11 = MaskF64(CmpF64(_mm256_loadu_pd(lhs + 44), _mm256_loadu_pd(rhs + 44), op));
    const uint64_t m12 = MaskF64(CmpF64(_mm256_loadu_pd(lhs + 48), _mm256_loadu_pd(rhs + 48), op));
    const uint64_t m13 = MaskF64(CmpF64(_mm256_loadu_pd(lhs + 52), _mm256_loadu_pd(rhs + 52), op));
    const uint64_t m14 = MaskF64(CmpF64(_mm256_loadu_pd(lhs + 56), _mm256_loadu_pd(rhs + 56), op));
    const uint64_t m15 = MaskF64(CmpF64(_mm256_loadu_pd(lhs + 60), _mm256_loadu_pd(rhs + 60), op));

    const uint64_t q0 = m0 | (m1 << 4) | (m2 << 8) | (m3 << 12);
    const uint64_t q1 = m4 | (m5 << 4) | (m6 << 8) | (m7 << 12);
    const uint64_t q2 = m8 | (m9 << 4) | (m10 << 8) | (m11 << 12);
    const uint64_t q3 = m12 | (m13 << 4) | (m14 << 8) | (m15 << 12);
    return q0 | (q1 << 16) | (q2 << 32) | (q3 << 48);
}

// ---------- const compare kernels ----------

void CompareI32ConstAvx2(const int32_t* data, uint32_t count, CmpOp op, int32_t rhs, SelectionMask& result) {
    result.SetNone(count);
    uint64_t* words = result.data();
    const __m256i vrhs = _mm256_set1_epi32(rhs);

    uint32_t i = 0;
    // full 64-row block：SetNone 已清零，整 word 直接覆盖（无 read-modify-write）
    for (; i + 64 <= count; i += 64) {
        words[i >> 6] = Compare64I32Const(data + i, vrhs, op);
    }
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
    for (; i + 64 <= count; i += 64) {
        words[i >> 6] = Compare64I64Const(data + i, vrhs, op);
    }
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
    for (; i + 64 <= count; i += 64) {
        words[i >> 6] = Compare64F32Const(data + i, vrhs, op);
    }
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
    for (; i + 64 <= count; i += 64) {
        words[i >> 6] = Compare64F64Const(data + i, vrhs, op);
    }
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
    for (; i + 64 <= count; i += 64) {
        words[i >> 6] = Compare64I32Column(lhs + i, rhs + i, op);
    }
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
    for (; i + 64 <= count; i += 64) {
        words[i >> 6] = Compare64I64Column(lhs + i, rhs + i, op);
    }
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
    for (; i + 64 <= count; i += 64) {
        words[i >> 6] = Compare64F32Column(lhs + i, rhs + i, op);
    }
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
    for (; i + 64 <= count; i += 64) {
        words[i >> 6] = Compare64F64Column(lhs + i, rhs + i, op);
    }
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
