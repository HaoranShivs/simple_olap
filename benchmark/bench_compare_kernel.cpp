// ============================================================
// AVX2 Compare 内核微基准：scalar / 新 64-row block / 旧逐段 |= 写法
// ============================================================
//
// 目的：度量 AVX2 compare kernel 的 64-row bitmap block 优化。
//
//   [scalar]  scalar backend（typed 逐行）
//   [avx2]    当前 AVX2 backend：64 行在寄存器拼一个 uint64_t，
//             每个 full word 只写一次
//   [old]     旧 AVX2 写法（本文件内复刻）：每 8/4 行对同一 word 做一次
//             read-modify-write（`words[base >> 6] |= mask << offset`）
//
// 覆盖 count = 16 / 64 / 256 / 1024（跨 64-row block 边界），
// 先逐 bit 验证 scalar == avx2 == old，再计时。
//
// 构建：
//   cmake --build build --target bench_compare_kernel
// 运行：
//   ./build/bin/bench_compare_kernel [--iters N] [--verify-only]

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "../src/simd/kernels.h"
#include "../src/simd/selection_mask.h"
#include "../src/type.h"

#if defined(__x86_64__) || defined(__i386__) || defined(_M_X64) || defined(_M_IX86)
#include <immintrin.h>
#define BENCH_HAS_X86 1
#else
#define BENCH_HAS_X86 0
#endif

using namespace simple_olap;
using namespace simple_olap::simd;

namespace {

using Clock = std::chrono::steady_clock;

constexpr uint32_t kN = 1024;

struct Args {
    uint32_t iters = 200000;
    bool verify_only = false;
};

Args ParseArgs(int argc, char** argv) {
    Args a;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--iters" && i + 1 < argc) {
            a.iters = static_cast<uint32_t>(std::stoul(argv[++i]));
        } else if (arg == "--verify-only") {
            a.verify_only = true;
        }
    }
    return a;
}

double TimeNs(void (*fn)(), uint32_t iters) {
    const auto t0 = Clock::now();
    for (uint32_t i = 0; i < iters; ++i) {
        fn();
    }
    const auto t1 = Clock::now();
    return static_cast<double>(std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count()) /
           static_cast<double>(iters);
}

// 全局输入/输出，供无捕获函数指针计时使用。
int32_t g_i32[kN];
int64_t g_i64[kN];
float g_f32[kN];
double g_f64[kN];
SelectionMask g_mask;
uint64_t g_sink = 0;

// 当前计时目标：类型 + count + 使用的 kernel。
const CompareKernels* g_scalar = nullptr;
const CompareKernels* g_avx2 = nullptr;
uint32_t g_count = kN;

#define DEFINE_TIMED(name, call_expr)                                                                              \
    void name() {                                                                                                  \
        call_expr;                                                                                                 \
        g_sink += g_mask.Count();                                                                                  \
    }

DEFINE_TIMED(TimeScalarI32, g_scalar->i32_const(g_i32, g_count, CmpOp::GT, 1234, g_mask))
DEFINE_TIMED(TimeAvx2I32, g_avx2->i32_const(g_i32, g_count, CmpOp::GT, 1234, g_mask))
DEFINE_TIMED(TimeScalarI64, g_scalar->i64_const(g_i64, g_count, CmpOp::GT, 9007199254740992LL, g_mask))
DEFINE_TIMED(TimeAvx2I64, g_avx2->i64_const(g_i64, g_count, CmpOp::GT, 9007199254740992LL, g_mask))
DEFINE_TIMED(TimeScalarF32, g_scalar->f32_const(g_f32, g_count, CmpOp::GT, 0.0f, g_mask))
DEFINE_TIMED(TimeAvx2F32, g_avx2->f32_const(g_f32, g_count, CmpOp::GT, 0.0f, g_mask))
DEFINE_TIMED(TimeScalarF64, g_scalar->f64_const(g_f64, g_count, CmpOp::GT, 0.0, g_mask))
DEFINE_TIMED(TimeAvx2F64, g_avx2->f64_const(g_f64, g_count, CmpOp::GT, 0.0, g_mask))

#if BENCH_HAS_X86
// ---------- 旧写法复刻：每 8/4 行 |= 一次同一 word ----------

__attribute__((target("avx2"))) void OldI32Const(const int32_t* data, uint32_t count, CmpOp op, int32_t rhs,
                                                 SelectionMask& result) {
    result.SetNone(count);
    uint64_t* words = result.data();
    const __m256i vrhs = _mm256_set1_epi32(rhs);
    uint32_t i = 0;
    for (; i + 8 <= count; i += 8) {
        const __m256i a = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(data + i));
        __m256i cmp;
        switch (op) {
        case CmpOp::EQ:
            cmp = _mm256_cmpeq_epi32(a, vrhs);
            break;
        case CmpOp::NE:
            cmp = _mm256_xor_si256(_mm256_cmpeq_epi32(a, vrhs), _mm256_set1_epi32(-1));
            break;
        case CmpOp::GT:
            cmp = _mm256_cmpgt_epi32(a, vrhs);
            break;
        case CmpOp::LT:
            cmp = _mm256_cmpgt_epi32(vrhs, a);
            break;
        case CmpOp::GE:
            cmp = _mm256_xor_si256(_mm256_cmpgt_epi32(vrhs, a), _mm256_set1_epi32(-1));
            break;
        case CmpOp::LE:
            cmp = _mm256_xor_si256(_mm256_cmpgt_epi32(a, vrhs), _mm256_set1_epi32(-1));
            break;
        }
        const uint32_t mask = static_cast<uint32_t>(_mm256_movemask_ps(_mm256_castsi256_ps(cmp)));
        words[i >> 6] |= uint64_t(mask) << (i & 63);
    }
    for (; i < count; ++i) {
        if (CompareTypedValue(data[i], op, rhs)) {
            words[i >> 6] |= (uint64_t(1) << (i & 63));
        }
    }
}

__attribute__((target("avx2"))) void OldF64Const(const double* data, uint32_t count, CmpOp op, double rhs,
                                                 SelectionMask& result) {
    result.SetNone(count);
    uint64_t* words = result.data();
    const __m256d vrhs = _mm256_set1_pd(rhs);
    uint32_t i = 0;
    for (; i + 4 <= count; i += 4) {
        const __m256d a = _mm256_loadu_pd(data + i);
        __m256d cmp;
        switch (op) {
        case CmpOp::EQ:
            cmp = _mm256_cmp_pd(a, vrhs, _CMP_EQ_OQ);
            break;
        case CmpOp::NE:
            cmp = _mm256_cmp_pd(a, vrhs, _CMP_NEQ_UQ);
            break;
        case CmpOp::GT:
            cmp = _mm256_cmp_pd(a, vrhs, _CMP_GT_OQ);
            break;
        case CmpOp::GE:
            cmp = _mm256_cmp_pd(a, vrhs, _CMP_GE_OQ);
            break;
        case CmpOp::LT:
            cmp = _mm256_cmp_pd(a, vrhs, _CMP_LT_OQ);
            break;
        case CmpOp::LE:
            cmp = _mm256_cmp_pd(a, vrhs, _CMP_LE_OQ);
            break;
        }
        const uint32_t mask = static_cast<uint32_t>(_mm256_movemask_pd(cmp));
        words[i >> 6] |= uint64_t(mask) << (i & 63);
    }
    for (; i < count; ++i) {
        if (CompareTypedValue(data[i], op, rhs)) {
            words[i >> 6] |= (uint64_t(1) << (i & 63));
        }
    }
}

DEFINE_TIMED(TimeOldI32, OldI32Const(g_i32, g_count, CmpOp::GT, 1234, g_mask))
DEFINE_TIMED(TimeOldF64, OldF64Const(g_f64, g_count, CmpOp::GT, 0.0, g_mask))
#endif // BENCH_HAS_X86

bool MaskEqualRaw(const SelectionMask& a, const SelectionMask& b, uint32_t count) {
    if (a.row_count() != b.row_count()) {
        return false;
    }
    for (uint32_t w = 0; w < SelectionMask::kWordCount; ++w) {
        if (a.data()[w] != b.data()[w]) {
            return false;
        }
    }
    (void)count;
    return true;
}

const char* OpName(CmpOp op) {
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

} // namespace

int main(int argc, char** argv) {
    const Args args = ParseArgs(argc, argv);

    const KernelRegistry& reg = KernelRegistry::Instance();
    const bool have_avx2 = (reg.backend() == SimdBackend::AVX2);
    std::printf("backend = %s, iters = %u\n\n", have_avx2 ? "AVX2" : "SCALAR", args.iters);

    CompareKernels scalar;
    InstallScalarCompareKernels(scalar);
    CompareKernels avx2;
    InstallScalarCompareKernels(avx2);
    InstallAvx2CompareKernels(avx2);

    g_scalar = &scalar;
    g_avx2 = &avx2;

    for (uint32_t i = 0; i < kN; ++i) {
        g_i32[i] = static_cast<int32_t>(i) * 7 - 3000;
        g_i64[i] = 9007199254740992LL + static_cast<int64_t>(i);
        g_f32[i] = static_cast<float>(i) * 0.5f - 100.0f;
        g_f64[i] = static_cast<double>(i) * 0.25 - 64.0;
    }

    const uint32_t counts[] = {16, 64, 256, 1024};
    const CmpOp ops[] = {CmpOp::EQ, CmpOp::NE, CmpOp::GT, CmpOp::GE, CmpOp::LT, CmpOp::LE};

    // ---------- 正确性校验 ----------
    for (uint32_t count : counts) {
        for (CmpOp op : ops) {
            SelectionMask sm;
            SelectionMask am;
            SelectionMask om;

            scalar.i32_const(g_i32, count, op, 1234, sm);
            avx2.i32_const(g_i32, count, op, 1234, am);
            if (!MaskEqualRaw(sm, am, count)) {
                std::printf("VERIFY FAILED: i32 %s count=%u\n", OpName(op), count);
                return 1;
            }
#if BENCH_HAS_X86
            if (have_avx2) {
                OldI32Const(g_i32, count, op, 1234, om);
                if (!MaskEqualRaw(om, am, count)) {
                    std::printf("VERIFY FAILED: old i32 %s count=%u\n", OpName(op), count);
                    return 1;
                }
            }
#endif

            scalar.f64_const(g_f64, count, op, 0.0, sm);
            avx2.f64_const(g_f64, count, op, 0.0, am);
            if (!MaskEqualRaw(sm, am, count)) {
                std::printf("VERIFY FAILED: f64 %s count=%u\n", OpName(op), count);
                return 1;
            }
#if BENCH_HAS_X86
            if (have_avx2) {
                OldF64Const(g_f64, count, op, 0.0, om);
                if (!MaskEqualRaw(om, am, count)) {
                    std::printf("VERIFY FAILED: old f64 %s count=%u\n", OpName(op), count);
                    return 1;
                }
            }
#endif
        }
    }
    std::printf("verify: scalar == avx2");
#if BENCH_HAS_X86
    std::printf(" == old");
#endif
    std::printf(" [OK]\n\n");

    if (args.verify_only) {
        return 0;
    }

    std::printf("%6s %-6s %14s %14s %14s %10s\n", "count", "type", "scalar(ns)", "avx2(ns)", "old(ns)", "speedup");
    std::printf("%s\n", std::string(72, '-').c_str());

    for (uint32_t count : counts) {
        g_count = count;

        struct Row {
            const char* type;
            void (*scalar_fn)();
            void (*avx2_fn)();
            void (*old_fn)();
        };

#if BENCH_HAS_X86
        void (*old_i32)() = have_avx2 ? &TimeOldI32 : nullptr;
        void (*old_f64)() = have_avx2 ? &TimeOldF64 : nullptr;
#else
        void (*old_i32)() = nullptr;
        void (*old_f64)() = nullptr;
#endif
        const Row rows[] = {
            {"i32", &TimeScalarI32, &TimeAvx2I32, old_i32},
            {"i64", &TimeScalarI64, &TimeAvx2I64, nullptr},
            {"f32", &TimeScalarF32, &TimeAvx2F32, nullptr},
            {"f64", &TimeScalarF64, &TimeAvx2F64, old_f64},
        };

        for (const Row& row : rows) {
            const double scalar_ns = TimeNs(row.scalar_fn, args.iters);
            const double avx2_ns = TimeNs(row.avx2_fn, args.iters);
            const double old_ns = row.old_fn != nullptr ? TimeNs(row.old_fn, args.iters) : 0.0;

            double speedup = 0.0;
            if (old_ns > 0.0 && avx2_ns > 0.0) {
                speedup = old_ns / avx2_ns;
            }

            if (old_ns > 0.0) {
                std::printf("%6u %-6s %14.0f %14.0f %14.0f %9.2fx\n", count, row.type, scalar_ns, avx2_ns, old_ns,
                            speedup);
            } else {
                std::printf("%6u %-6s %14.0f %14.0f %14s %10s\n", count, row.type, scalar_ns, avx2_ns, "-", "-");
            }
        }
    }

    (void)g_sink;
    return 0;
}
