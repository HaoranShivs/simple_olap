#include "cpu_features.h"

#include <cstdlib>

namespace simple_olap::simd {

namespace {

// 运行时后端覆盖：SIMPLE_OLAP_FORCE_SCALAR=1 时强制关闭 AVX2，
// 用于在同一个二进制上对比 scalar / AVX2 两条执行路径（benchmark）。
// 只在 KernelRegistry 初始化时读取一次，不进入热路径。
bool ForceScalarRequested() {
    const char* value = std::getenv("SIMPLE_OLAP_FORCE_SCALAR");
    if (value == nullptr || value[0] == '\0') {
        return false;
    }
    return value[0] != '0';
}

} // namespace

CpuFeatures CpuFeatures::Detect() {
    CpuFeatures features;

    if (ForceScalarRequested()) {
        return features; // avx2 = false，强制走 scalar backend
    }

#if defined(__x86_64__) || defined(__i386__) || defined(_M_X64) || defined(_M_IX86)
#if defined(__GNUC__) || defined(__clang__)
    // __builtin_cpu_init 必须在 __builtin_cpu_supports 之前调用（首次）。
    __builtin_cpu_init();
    features.avx2 = __builtin_cpu_supports("avx2");
#endif
#endif

    return features;
}

} // namespace simple_olap::simd
