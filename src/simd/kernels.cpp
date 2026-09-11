#include "kernels.h"

namespace simple_olap::simd {

const KernelRegistry& KernelRegistry::Instance() {
    // C++11 起函数内 static 初始化线程安全。
    static const KernelRegistry instance;
    return instance;
}

KernelRegistry::KernelRegistry() : cpu_features_(CpuFeatures::Detect()) {
    // 先装 scalar：保证任何平台上热路径都有可用实现。
    InstallScalarCompareKernels(compare_);

    // AVX2 可用时再覆盖为向量实现。
    if (cpu_features_.avx2) {
        InstallAvx2CompareKernels(compare_);
        backend_ = SimdBackend::AVX2;
    } else {
        backend_ = SimdBackend::SCALAR;
    }
}

} // namespace simple_olap::simd
