#include "../gather_kernel.h"

#include <cstring>

#include "../../type.h"

// 本 translation unit 在 CMake 中被单独施加 -mavx2；非 x86 平台不施加该选项，
// 且下面的实现体整体被条件编译屏蔽，只保留空的 InstallAvx2GatherKernels。

#if defined(__x86_64__) || defined(__i386__) || defined(_M_X64) || defined(_M_IX86)

#include <immintrin.h>

namespace simple_olap::simd {
namespace {

// ---------- 设计说明（基于实测，而非教条） ----------
//
// 这些内核刻意不使用硬件 gather 指令（vgatherdps/vgatherdpd）：
// 在主流 x86（含 Alder Lake P-core）上，8/4 元素硬件 gather 是微码实现，
// 吞吐远差于「多次独立 load + 一条向量 store」的软件 gather。
// 实测（1024 行、50% 随机选择率，ns/row）：
//   f64: 硬件 gather 1.49 / scalar 0.26 / 软件 gather 0.61
//   i64: 硬件 gather 1.45 / scalar 0.24
//   f32: 硬件 gather 0.87 / scalar 0.61 / 软件 gather 0.43
//   i32: 硬件 gather 0.85 / scalar 0.47 / 软件 gather 0.43
//
// 因此 AVX2 backend 只覆盖 32-bit 类型（8 路软件 gather 确实更快）；
// 64-bit 类型保留 scalar 实现（4 路独立 load 更快，
// InstallAvx2GatherKernels 不覆盖它们）。
// 将来若目标 CPU 的硬件 gather 明显更快，替换这里的实现即可，
// 上层通过 GatherKernels 函数表调用，无需改动。

// ---------- INT32：一次 8 行 ----------

void GatherI32Avx2(const int32_t* src, const SelectionMask& selection, int32_t* dst) {
    SelectionCursor cursor(selection);
    uint32_t out = 0;
    alignas(32) uint32_t indices[8];

    for (;;) {
        const uint32_t n = cursor.NextBlock(indices, 8);
        if (n == 0) {
            break;
        }
        if (n == 8) {
            const __m256i result =
                _mm256_set_epi32(src[indices[7]], src[indices[6]], src[indices[5]], src[indices[4]],
                                 src[indices[3]], src[indices[2]], src[indices[1]], src[indices[0]]);
            _mm256_storeu_si256(reinterpret_cast<__m256i*>(dst + out), result);
            out += 8;
        } else {
            for (uint32_t i = 0; i < n; ++i) {
                dst[out++] = src[indices[i]];
            }
            break;
        }
    }
}

// ---------- FLOAT：一次 8 行 ----------

void GatherF32Avx2(const float* src, const SelectionMask& selection, float* dst) {
    SelectionCursor cursor(selection);
    uint32_t out = 0;
    alignas(32) uint32_t indices[8];

    for (;;) {
        const uint32_t n = cursor.NextBlock(indices, 8);
        if (n == 0) {
            break;
        }
        if (n == 8) {
            const __m256 result =
                _mm256_set_ps(src[indices[7]], src[indices[6]], src[indices[5]], src[indices[4]],
                              src[indices[3]], src[indices[2]], src[indices[1]], src[indices[0]]);
            _mm256_storeu_ps(dst + out, result);
            out += 8;
        } else {
            for (uint32_t i = 0; i < n; ++i) {
                dst[out++] = src[indices[i]];
            }
            break;
        }
    }
}

// ---------- VARCHAR：固定 64-byte slot，AVX2 无收益，保持 memcpy ----------

void GatherVarcharAvx2(const uint8_t* src, const SelectionMask& selection, uint8_t* dst) {
    uint32_t out = 0;
    selection.ForEachSetBit([&](uint32_t row) {
        std::memcpy(dst + static_cast<size_t>(out) * kVarcharSlotSize,
                    src + static_cast<size_t>(row) * kVarcharSlotSize, kVarcharSlotSize);
        ++out;
    });
}

} // namespace
} // namespace simple_olap::simd

#endif // x86

namespace simple_olap::simd {

void InstallAvx2GatherKernels(GatherKernels& kernels) {
#if defined(__x86_64__) || defined(__i386__) || defined(_M_X64) || defined(_M_IX86)
    // 只覆盖 32-bit 类型；i64/f64 保留 scalar 实现（见文件头实测说明）。
    kernels.i32 = &GatherI32Avx2;
    kernels.f32 = &GatherF32Avx2;
    kernels.varchar = &GatherVarcharAvx2;
#else
    (void)kernels;
#endif
}

} // namespace simple_olap::simd
