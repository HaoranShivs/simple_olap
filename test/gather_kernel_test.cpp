// Gather kernel 验证程序（手工编译或通过 CMake test target 构建）。
//
// 编译（在项目根目录）：
//   g++ -std=c++17 -O2 -Isrc test/gather_kernel_test.cpp \
//       build/lib/libsimple_olap_core.a -lpthread -o /tmp/gather_kernel_test
//
// 验证内容：
//   1. INT32 / INT64 / FLOAT / DOUBLE / VARCHAR 的 scalar gather
//      与 ForEachSetBit 逐行语义一致
//   2. AVX2 gather 与 scalar gather 逐字节一致
//   3. 覆盖 0% / 1% / 10% / 25% / 50% / 75% / 90% / 99% / 100% 选择率
//      （含空 selection、全选、跨 word 边界）

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "simd/gather_kernel.h"
#include "simd/kernels.h"
#include "simd/selection_mask.h"
#include "type.h"

using namespace simple_olap;
using namespace simple_olap::simd;

static int g_failures = 0;

static void Fail(const char* label) {
    std::printf("FAIL: %s\n", label);
    ++g_failures;
}

static constexpr uint32_t kN = 1024;

// 确定性选择率掩码：percent% 的行被选中。
static void BuildMask(SelectionMask& mask, uint32_t n, uint32_t percent) {
    mask.SetNone(n);
    for (uint32_t row = 0; row < n; ++row) {
        if ((row * 2654435761u) % 100u < percent) {
            mask.data()[row >> 6] |= (uint64_t(1) << (row & 63));
        }
    }
}

template <typename T, typename Fn>
static void TestTypedGather(const char* name, const std::vector<T>& src, Fn scalar_fn, Fn avx2_fn, bool have_avx2,
                            uint32_t percent) {
    SelectionMask mask;
    BuildMask(mask, static_cast<uint32_t>(src.size()), percent);

    std::vector<T> expected;
    mask.ForEachSetBit([&](uint32_t row) { expected.push_back(src[row]); });

    std::vector<T> out_scalar(expected.size());
    std::vector<T> out_avx2(expected.size());

    scalar_fn(src.data(), mask, out_scalar.data());
    if (out_scalar != expected) {
        char buf[128];
        std::snprintf(buf, sizeof(buf), "%s scalar gather mismatch at %u%%", name, percent);
        Fail(buf);
    }

    if (have_avx2) {
        avx2_fn(src.data(), mask, out_avx2.data());
        if (out_avx2 != expected) {
            char buf[128];
            std::snprintf(buf, sizeof(buf), "%s avx2 gather mismatch at %u%%", name, percent);
            Fail(buf);
        }
    }
}

static void TestVarcharGather(const GatherKernels& scalar, const GatherKernels& avx2, bool have_avx2,
                              uint32_t percent) {
    std::vector<uint8_t> src(static_cast<size_t>(kN) * kVarcharSlotSize);
    for (uint32_t row = 0; row < kN; ++row) {
        std::string value = "row_" + std::to_string(row) + "_payload";
        WriteVarcharSlot(src.data() + static_cast<size_t>(row) * kVarcharSlotSize, value);
    }

    SelectionMask mask;
    BuildMask(mask, kN, percent);

    std::vector<uint8_t> expected(static_cast<size_t>(mask.Count()) * kVarcharSlotSize);
    {
        uint32_t out = 0;
        mask.ForEachSetBit([&](uint32_t row) {
            std::memcpy(expected.data() + static_cast<size_t>(out) * kVarcharSlotSize,
                        src.data() + static_cast<size_t>(row) * kVarcharSlotSize, kVarcharSlotSize);
            ++out;
        });
    }

    std::vector<uint8_t> out_scalar(expected.size());
    std::vector<uint8_t> out_avx2(expected.size());
    scalar.varchar(src.data(), mask, out_scalar.data());
    if (out_scalar != expected) {
        Fail("varchar scalar gather mismatch");
    }
    if (have_avx2) {
        avx2.varchar(src.data(), mask, out_avx2.data());
        if (out_avx2 != expected) {
            Fail("varchar avx2 gather mismatch");
        }
    }
}

int main() {
    const KernelRegistry& reg = KernelRegistry::Instance();
    const bool have_avx2 = (reg.backend() == SimdBackend::AVX2);
    std::printf("backend = %s\n", have_avx2 ? "AVX2" : "SCALAR");

    GatherKernels scalar;
    InstallScalarGatherKernels(scalar);
    GatherKernels avx2;
    InstallScalarGatherKernels(avx2);
    InstallAvx2GatherKernels(avx2);

    std::vector<int32_t> i32(kN);
    std::vector<int64_t> i64(kN);
    std::vector<float> f32(kN);
    std::vector<double> f64(kN);
    const int64_t base64 = 9007199254740992LL; // 2^53：超过 double 精确表示范围
    for (uint32_t i = 0; i < kN; ++i) {
        i32[i] = static_cast<int32_t>(i) * 7 - 3000;
        i64[i] = base64 + static_cast<int64_t>(i);
        f32[i] = static_cast<float>(i) * 0.5f - 100.0f;
        f64[i] = static_cast<double>(i) * 0.25 - 64.0;
    }

    const uint32_t percents[] = {0, 1, 10, 25, 50, 75, 90, 99, 100};
    for (uint32_t p : percents) {
        TestTypedGather<int32_t>("int32", i32, scalar.i32, avx2.i32, have_avx2, p);
        TestTypedGather<int64_t>("int64", i64, scalar.i64, avx2.i64, have_avx2, p);
        TestTypedGather<float>("float", f32, scalar.f32, avx2.f32, have_avx2, p);
        TestTypedGather<double>("double", f64, scalar.f64, avx2.f64, have_avx2, p);
        TestVarcharGather(scalar, avx2, have_avx2, p);
    }

    // 非 64 整数倍的尾部（33 / 65 / 100 行），覆盖 NextBlock 尾部标量路径。
    for (uint32_t n : {33u, 65u, 100u}) {
        SelectionMask mask;
        BuildMask(mask, n, 50);
        std::vector<int64_t> expected;
        mask.ForEachSetBit([&](uint32_t row) { expected.push_back(i64[row]); });
        std::vector<int64_t> out(expected.size());
        scalar.i64(i64.data(), mask, out.data());
        if (out != expected) {
            Fail("int64 tail scalar gather mismatch");
        }
        if (have_avx2) {
            std::vector<int64_t> out2(expected.size());
            avx2.i64(i64.data(), mask, out2.data());
            if (out2 != expected) {
                Fail("int64 tail avx2 gather mismatch");
            }
        }
    }

    if (g_failures == 0) {
        std::printf("ALL GATHER KERNEL TESTS PASSED\n");
        return 0;
    }
    std::printf("%d FAILURE(S)\n", g_failures);
    return 1;
}
