// SIMD compare kernel 验证程序（手工编译，不参与库构建）。
//
// 编译（在项目根目录）：
//   g++ -std=c++17 -O2 -Isrc test/simd_compare_test.cpp \
//       build/lib/libsimple_olap_core.a -lpthread -o /tmp/simd_compare_test
//
// 验证内容：
//   1. KernelRegistry 后端选择与 AVX2 探测一致
//   2. SelectionMask 基本不变式（SetAll/SetNone/Count/And/Or/AndNot/ToSelectionVector）
//   3. scalar 与 AVX2 CompareKernel 对全部 CmpOp × 4 类型结果一致
//   4. INT64 > 2^53 时不再经过 double（精度修复验证）

#include <cstdint>
#include <cstdio>
#include <vector>

#include "simd/kernels.h"
#include "simd/selection_mask.h"

using namespace simple_olap;
using namespace simple_olap::simd;

static int g_failures = 0;

static void Fail(const char* label) {
    std::printf("FAIL: %s\n", label);
    ++g_failures;
}

static bool MaskEquals(const SelectionMask& a, const SelectionMask& b, uint32_t count) {
    if (a.row_count() != b.row_count()) {
        return false;
    }
    for (uint32_t i = 0; i < count; ++i) {
        if (a.Test(i) != b.Test(i)) {
            return false;
        }
    }
    return true;
}

static const CmpOp kOps[] = {CmpOp::EQ, CmpOp::NE, CmpOp::GT, CmpOp::GE, CmpOp::LT, CmpOp::LE};
static const char* OpName(CmpOp op) {
    switch (op) {
    case CmpOp::EQ:
        return "EQ";
    case CmpOp::NE:
        return "NE";
    case CmpOp::GT:
        return "GT";
    case CmpOp::GE:
        return "GE";
    case CmpOp::LT:
        return "LT";
    case CmpOp::LE:
        return "LE";
    }
    return "?";
}

// count 取非向量整数倍，覆盖尾部标量路径（i32:8, i64:4, f32:8, f64:4）
static constexpr uint32_t kN = 1024;

// ---------- SelectionMask 遍历（ForEachSetBit / SelectionCursor / NextBlock） ----------

// 9 种确定性行分布：ALL / NONE / 单 bit / 交替 / 随机 10% / 50% / 90% / 最后一位 / 跨 word 边界
static constexpr int kPatternCount = 9;

static bool PatternSelected(int pattern, uint32_t row, uint32_t n) {
    switch (pattern) {
    case 0:
        return true;
    case 1:
        return false;
    case 2:
        return n > 0 && row == n / 2;
    case 3:
        return (row % 2) == 0;
    case 4:
        return (row * 2654435761u) % 100u < 10u;
    case 5:
        return (row * 2654435761u) % 100u < 50u;
    case 6:
        return (row * 2654435761u) % 100u < 90u;
    case 7:
        return n > 0 && row + 1 == n;
    case 8:
        return (row % 64u) == 0 || (row % 64u) == 63;
    }
    return false;
}

static void TestMaskIteration() {
    const uint32_t counts[] = {0, 1, 4, 7, 8, 31, 63, 64, 65, 127, 128, 1023, 1024};

    for (uint32_t n : counts) {
        for (int pattern = 0; pattern < kPatternCount; ++pattern) {
            std::vector<uint32_t> expected;
            SelectionMask mask;
            mask.SetNone(n);
            for (uint32_t row = 0; row < n; ++row) {
                if (PatternSelected(pattern, row, n)) {
                    mask.data()[row >> 6] |= (uint64_t(1) << (row & 63));
                    expected.push_back(row);
                }
            }

            if (mask.Count() != expected.size()) {
                Fail("mask iteration: Count mismatch");
            }
            if (mask.Empty() != expected.empty()) {
                Fail("mask iteration: Empty mismatch");
            }
            if (mask.IsAll() != (n == 0 || expected.size() == n)) {
                Fail("mask iteration: IsAll mismatch");
            }

            // ForEachSetBit 与 ToSelectionVector 必须一致
            std::vector<uint32_t> got;
            mask.ForEachSetBit([&](uint32_t row) { got.push_back(row); });
            if (got != expected) {
                Fail("mask iteration: ForEachSetBit mismatch");
            }

            std::vector<uint32_t> sv;
            mask.ToSelectionVector(sv);
            if (sv != expected) {
                Fail("mask iteration: ToSelectionVector mismatch");
            }

            // SelectionCursor::Next
            std::vector<uint32_t> cursor_rows;
            SelectionCursor cursor = mask.MakeCursor();
            uint32_t row = 0;
            while (cursor.Next(row)) {
                cursor_rows.push_back(row);
            }
            if (cursor_rows != expected) {
                Fail("mask iteration: SelectionCursor::Next mismatch");
            }

            // SelectionCursor::NextBlock：各种容量都必须产出同一序列
            const uint32_t capacities[] = {1, 2, 3, 5, 7, 8, 9, 16, 64, 1024};
            for (uint32_t cap : capacities) {
                std::vector<uint32_t> block(cap);
                std::vector<uint32_t> block_rows;
                SelectionCursor block_cursor = mask.MakeCursor();
                for (;;) {
                    const uint32_t emitted = block_cursor.NextBlock(block.data(), cap);
                    if (emitted == 0) {
                        break;
                    }
                    block_rows.insert(block_rows.end(), block.begin(), block.begin() + emitted);
                }
                if (block_rows != expected) {
                    Fail("mask iteration: SelectionCursor::NextBlock mismatch");
                }
            }
        }
    }
}

// ---------- 常量比较：scalar vs expected ----------
template <typename T>
static void TestConst(const char* type_name, const CompareKernels& scalar, const CompareKernels& avx2, bool have_avx2,
                      const T* data, uint32_t n, T rhs, void (*scalar_fn)(const T*, uint32_t, CmpOp, T, SelectionMask&),
                      void (*avx2_fn)(const T*, uint32_t, CmpOp, T, SelectionMask&)) {
    for (CmpOp op : kOps) {
        SelectionMask sm;
        SelectionMask am;
        scalar_fn(data, n, op, rhs, sm);
        avx2_fn(data, n, op, rhs, am);

        // 1) scalar 与逐元素语义一致
        for (uint32_t i = 0; i < n; ++i) {
            if (sm.Test(i) != CompareTypedValue<T>(data[i], op, rhs)) {
                char buf[128];
                std::snprintf(buf, sizeof(buf), "%s const %s scalar != typed semantics (row %u)", type_name, OpName(op),
                              i);
                Fail(buf);
                break;
            }
        }

        // 2) AVX2 与 scalar 一致
        if (have_avx2 && !MaskEquals(sm, am, n)) {
            char buf[128];
            std::snprintf(buf, sizeof(buf), "%s const %s avx2 != scalar", type_name, OpName(op));
            Fail(buf);
        }

        // 3) 末尾 bit 必须为 0
        if (have_avx2) {
            for (uint32_t i = n; i < n + 8 && i < SelectionMask::kWordCount * SelectionMask::kWordBits; ++i) {
                if (am.Test(i)) {
                    Fail("avx2 tail bits not cleared");
                    break;
                }
            }
        }
    }
}

int main() {
    const KernelRegistry& reg = KernelRegistry::Instance();
    const bool have_avx2 = (reg.backend() == SimdBackend::AVX2);
    std::printf("backend = %s\n", have_avx2 ? "AVX2" : "SCALAR");

    // 独立构造两张表用于对比
    CompareKernels scalar;
    InstallScalarCompareKernels(scalar);
    CompareKernels avx2;
    InstallScalarCompareKernels(avx2);
    InstallAvx2CompareKernels(avx2);

    // ---------- SelectionMask ----------
    {
        // 非 64 整数倍的 SetAll：IsAll 必须忽略超出 row_count_ 的位
        for (uint32_t n : {1u, 63u, 64u, 65u, 100u, 1024u}) {
            SelectionMask m;
            m.SetAll(n);
            if (m.Count() != n || !m.IsAll() || (n > 0 && m.Empty())) {
                Fail("SelectionMask SetAll non-multiple");
            }
            m.SetNone(n);
            if (m.Count() != 0 || !m.Empty()) {
                Fail("SelectionMask SetNone");
            }
        }

        SelectionMask a;
        a.SetNone(128);
        a.data()[0] = 0b1111ULL;
        a.data()[1] = 0b1ULL;
        if (a.Count() != 5) {
            Fail("SelectionMask Count");
        }

        SelectionMask b;
        b.SetNone(128);
        b.data()[0] = 0b1100ULL;
        a.And(b);
        std::vector<uint32_t> out;
        a.ToSelectionVector(out);
        const std::vector<uint32_t> expect_and = {2, 3};
        if (out != expect_and) {
            Fail("SelectionMask And");
        }
    }

    // ---------- SelectionMask 遍历 / SelectionCursor ----------
    TestMaskIteration();

    // ---------- INT32 ----------
    std::vector<int32_t> i32(kN);
    for (uint32_t i = 0; i < kN; ++i) {
        i32[i] = static_cast<int32_t>(i) * 7 - 3000;
    }
    TestConst<int32_t>("int32", scalar, avx2, have_avx2, i32.data(), kN, 1234, scalar.i32_const, avx2.i32_const);
    TestConst<int32_t>("int32", scalar, avx2, have_avx2, i32.data(), kN, -3000, scalar.i32_const, avx2.i32_const);

    // ---------- INT64（含 > 2^53 精度边界） ----------
    std::vector<int64_t> i64(kN);
    const int64_t base = 9007199254740992LL; // 2^53
    for (uint32_t i = 0; i < kN; ++i) {
        i64[i] = base + static_cast<int64_t>(i); // base+1 等无法用 double 精确表示
    }
    TestConst<int64_t>("int64", scalar, avx2, have_avx2, i64.data(), kN, base, scalar.i64_const, avx2.i64_const);
    TestConst<int64_t>("int64", scalar, avx2, have_avx2, i64.data(), kN, base + 1, scalar.i64_const, avx2.i64_const);

    {
        // 旧实现把 int64 转 double：base 与 base+1 会相等，导致 GT 选空。
        // 新实现必须只选中 base+1 起的行。
        SelectionMask m;
        reg.compare().i64_const(i64.data(), kN, CmpOp::GT, base, m);
        if (m.Count() != kN - 1) {
            Fail("int64 GT precision (2^53 boundary)");
        }
        if (!m.Test(1) || m.Test(0)) {
            Fail("int64 GT exact row selection");
        }
    }

    // ---------- FLOAT ----------
    std::vector<float> f32(kN);
    for (uint32_t i = 0; i < kN; ++i) {
        f32[i] = static_cast<float>(i) * 0.5f - 100.0f;
    }
    TestConst<float>("float", scalar, avx2, have_avx2, f32.data(), kN, 0.5f, scalar.f32_const, avx2.f32_const);

    // ---------- DOUBLE ----------
    std::vector<double> f64(kN);
    for (uint32_t i = 0; i < kN; ++i) {
        f64[i] = static_cast<double>(i) * 0.25 - 64.0;
    }
    TestConst<double>("double", scalar, avx2, have_avx2, f64.data(), kN, 0.0, scalar.f64_const, avx2.f64_const);

    // ---------- 非整数倍 count（尾部路径） ----------
    {
        const uint32_t n = 33;
        SelectionMask sm;
        SelectionMask am;
        scalar.i32_const(i32.data(), n, CmpOp::GE, 0, sm);
        if (have_avx2) {
            avx2.i32_const(i32.data(), n, CmpOp::GE, 0, am);
            if (!MaskEquals(sm, am, n)) {
                Fail("int32 tail count mismatch");
            }
        }
    }

    if (g_failures == 0) {
        std::printf("ALL SIMD COMPARE TESTS PASSED\n");
        return 0;
    }
    std::printf("%d FAILURE(S)\n", g_failures);
    return 1;
}
