#include "../gather_kernel.h"

#include <cstring>

#include "../../type.h"

namespace simple_olap::simd {
namespace {

template <typename T> void GatherScalar(const T* src, const SelectionMask& selection, T* dst) {
    uint32_t out = 0;
    selection.ForEachSetBit([&](uint32_t row) {
        dst[out] = src[row];
        ++out;
    });
}

void GatherVarcharScalar(const uint8_t* src, const SelectionMask& selection, uint8_t* dst) {
    uint32_t out = 0;
    selection.ForEachSetBit([&](uint32_t row) {
        std::memcpy(dst + static_cast<size_t>(out) * kVarcharSlotSize,
                    src + static_cast<size_t>(row) * kVarcharSlotSize, kVarcharSlotSize);
        ++out;
    });
}

} // namespace

void InstallScalarGatherKernels(GatherKernels& kernels) {
    kernels.i32 = &GatherScalar<int32_t>;
    kernels.i64 = &GatherScalar<int64_t>;
    kernels.f32 = &GatherScalar<float>;
    kernels.f64 = &GatherScalar<double>;
    kernels.varchar = &GatherVarcharScalar;
}

} // namespace simple_olap::simd
