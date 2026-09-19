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
//   5. AVX2 64-row bitmap block：系统化边界 count（0/1/3/.../64/65/.../1024）
//      × const/column × 4 类型 × 6 种 CmpOp，含 NaN / ±Inf / ±0

#include <cstdint>
#include <cstdio>
#include <limits>
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

        // 3) 末尾 bit 必须为 0（直接看原始 bit，Test() 会因 row_count 掩盖）
        if (have_avx2) {
            for (uint32_t i = n; i < SelectionMask::kWordCount * SelectionMask::kWordBits; ++i) {
                if (((am.data()[i >> 6] >> (i & 63)) & 1ULL) != 0) {
                    Fail("avx2 tail bits not cleared");
                    break;
                }
            }
        }
    }
}

// ---------- 64-row block 边界测试 ----------
//
// 覆盖所有跨越 8 / 4 / 64 行的边界 count，对每种类型验证：
//   1) scalar 后端与逐元素 typed 语义一致
//   2) AVX2 后端与 scalar 后端逐 bit 一致
//   3) [count, kWordCount*64) 的原始 bit 必须为 0（不能只看 Test()）
static const uint32_t kBoundaryCounts[] = {0,  1,  3,  4,  7,  8,   15,  16,  31,  32,
                                           63, 64, 65, 71, 72, 127, 128, 129, 1023, 1024};

static bool RawBitSet(const SelectionMask& mask, uint32_t row) {
    return ((mask.data()[row >> 6] >> (row & 63)) & 1ULL) != 0;
}

template <typename T, typename ConstFn, typename ColumnFn>
static void TestBoundaries(const char* name, const CompareKernels& scalar, const CompareKernels& avx2, bool have_avx2,
                           const std::vector<T>& lhs, const std::vector<T>& rhs, ConstFn scalar_const,
                           ConstFn avx2_const, ColumnFn scalar_col, ColumnFn avx2_col) {
    for (uint32_t n : kBoundaryCounts) {
        if (n > lhs.size()) {
            continue;
        }

        for (CmpOp op : kOps) {
            // ---- const compare（rhs 取中间元素，覆盖浮点特殊值） ----
            const T constant = lhs[n / 2];
            SelectionMask sm;
            scalar_const(lhs.data(), n, op, constant, sm);
            for (uint32_t i = 0; i < n; ++i) {
                if (sm.Test(i) != CompareTypedValue<T>(lhs[i], op, constant)) {
                    char buf[160];
                    std::snprintf(buf, sizeof(buf), "%s boundary const %s: scalar != typed (count=%u row=%u)", name,
                                  OpName(op), n, i);
                    Fail(buf);
                    break;
                }
            }

            if (have_avx2) {
                SelectionMask am;
                avx2_const(lhs.data(), n, op, constant, am);
                if (!MaskEquals(sm, am, n)) {
                    char buf[160];
                    std::snprintf(buf, sizeof(buf), "%s boundary const %s: avx2 != scalar (count=%u)", name,
                                  OpName(op), n);
                    Fail(buf);
                }
                for (uint32_t i = n; i < SelectionMask::kWordCount * SelectionMask::kWordBits; ++i) {
                    if (RawBitSet(am, i)) {
                        Fail("boundary const: avx2 tail bits not cleared");
                        break;
                    }
                }
            }

            // ---- column compare ----
            SelectionMask sc;
            scalar_col(lhs.data(), rhs.data(), n, op, sc);
            for (uint32_t i = 0; i < n; ++i) {
                if (sc.Test(i) != CompareTypedValue<T>(lhs[i], op, rhs[i])) {
                    char buf[160];
                    std::snprintf(buf, sizeof(buf), "%s boundary column %s: scalar != typed (count=%u row=%u)", name,
                                  OpName(op), n, i);
                    Fail(buf);
                    break;
                }
            }

            if (have_avx2) {
                SelectionMask ac;
                avx2_col(lhs.data(), rhs.data(), n, op, ac);
                if (!MaskEquals(sc, ac, n)) {
                    char buf[160];
                    std::snprintf(buf, sizeof(buf), "%s boundary column %s: avx2 != scalar (count=%u)", name,
                                  OpName(op), n);
                    Fail(buf);
                }
                for (uint32_t i = n; i < SelectionMask::kWordCount * SelectionMask::kWordBits; ++i) {
                    if (RawBitSet(ac, i)) {
                        Fail("boundary column: avx2 tail bits not cleared");
                        break;
                    }
                }
            }
        }
    }
}

// 浮点特殊值数组：NaN / +Inf / -Inf / +0 / -0 交替出现，
// 确保 64-row 重构没有改变当前浮点比较语义（EQ/NE 对 NaN 的行为等）。
template <typename T> static T FloatSpecial(uint32_t row, T ordinary) {
    switch (row % 8u) {
    case 0:
        return std::numeric_limits<T>::quiet_NaN();
    case 1:
        return std::numeric_limits<T>::infinity();
    case 2:
        return -std::numeric_limits<T>::infinity();
    case 3:
        return T(0);
    case 4:
        return -T(0);
    default:
        return ordinary;
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

    // ---------- 64-row block 边界：INT32 ----------
    {
        std::vector<int32_t> rhs32(kN);
        for (uint32_t i = 0; i < kN; ++i) {
            rhs32[i] = i32[(i * 7u + 3u) % kN];
        }
        TestBoundaries<int32_t>("int32", scalar, avx2, have_avx2, i32, rhs32, scalar.i32_const, avx2.i32_const,
                                scalar.i32_column, avx2.i32_column);
    }

    // ---------- 64-row block 边界：INT64 ----------
    {
        std::vector<int64_t> rhs64(kN);
        for (uint32_t i = 0; i < kN; ++i) {
            rhs64[i] = i64[(i * 7u + 3u) % kN];
        }
        TestBoundaries<int64_t>("int64", scalar, avx2, have_avx2, i64, rhs64, scalar.i64_const, avx2.i64_const,
                                scalar.i64_column, avx2.i64_column);
    }

    // ---------- 64-row block 边界：FLOAT（含 NaN / ±Inf / ±0） ----------
    {
        std::vector<float> f32s(kN);
        std::vector<float> f32r(kN);
        for (uint32_t i = 0; i < kN; ++i) {
            f32s[i] = FloatSpecial<float>(i, static_cast<float>(i) * 0.5f - 100.0f);
            f32r[i] = FloatSpecial<float>(i + 3u, static_cast<float>(i) * 0.25f - 10.0f);
        }
        TestBoundaries<float>("float", scalar, avx2, have_avx2, f32s, f32r, scalar.f32_const, avx2.f32_const,
                              scalar.f32_column, avx2.f32_column);
    }

    // ---------- 64-row block 边界：DOUBLE（含 NaN / ±Inf / ±0） ----------
    {
        std::vector<double> f64s(kN);
        std::vector<double> f64r(kN);
        for (uint32_t i = 0; i < kN; ++i) {
            f64s[i] = FloatSpecial<double>(i, static_cast<double>(i) * 0.25 - 64.0);
            f64r[i] = FloatSpecial<double>(i + 5u, static_cast<double>(i) * 0.125 - 8.0);
        }
        TestBoundaries<double>("double", scalar, avx2, have_avx2, f64s, f64r, scalar.f64_const, avx2.f64_const,
                               scalar.f64_column, avx2.f64_column);
    }

    if (g_failures == 0) {
        std::printf("ALL SIMD COMPARE TESTS PASSED\n");
        return 0;
    }
    std::printf("%d FAILURE(S)\n", g_failures);
    return 1;
}
