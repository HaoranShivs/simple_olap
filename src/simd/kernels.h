#pragma once

#include "compare_kernel.h"
#include "cpu_features.h"

namespace simple_olap::simd {

// 安装函数：由各 backend 的 translation unit 提供。
// scalar 版总是可用；avx2 版在非 x86 平台上是 no-op（不会被安装）。
void InstallScalarCompareKernels(CompareKernels& kernels);
void InstallAvx2CompareKernels(CompareKernels& kernels);

// KernelRegistry：进程级单例，初始化时做一次 CPU 探测 + 后端选择，
// 之后热路径只查表调用函数指针，不再执行 CPUID。
//
// 初始化：
//   CPU supports AVX2
//        ├── yes -> InstallScalar + InstallAvx2
//        └── no  -> InstallScalar
class KernelRegistry {
  public:
    static const KernelRegistry& Instance();

    KernelRegistry(const KernelRegistry&) = delete;
    KernelRegistry& operator=(const KernelRegistry&) = delete;

    SimdBackend backend() const noexcept {
        return backend_;
    }

    const CompareKernels& compare() const noexcept {
        return compare_;
    }

  private:
    KernelRegistry();

    CpuFeatures cpu_features_;
    SimdBackend backend_ = SimdBackend::SCALAR;
    CompareKernels compare_;
};

} // namespace simple_olap::simd
