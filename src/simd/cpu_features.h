#pragma once

#include <cstdint>

namespace simple_olap::simd {

// SIMD 后端标识。
enum class SimdBackend : uint8_t {
    SCALAR = 0,
    AVX2,
};

// 一次性探测的 CPU 特性。只在 KernelRegistry 初始化时调用 Detect()。
struct CpuFeatures {
    bool avx2 = false;

    // 仅在支持的平台上真正探测；其他平台返回全 false。
    static CpuFeatures Detect();
};

} // namespace simple_olap::simd
