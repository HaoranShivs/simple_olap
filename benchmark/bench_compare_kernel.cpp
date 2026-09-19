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
// 计时修正（BenchmarkSettingV2）：
//   旧实现把 `g_sink += g_mask.Count()` 放进 timed function，等于把一次
//   popcount 成本混进 kernel；count=16/64 时占比可观。现在 Count() 完全
//   移出计时区间，只用 DoNotOptimize 阻止 mask 被优化掉。
//
// 计时使用统一 harness：warmup + samples + median/MAD + 固定种子随机顺序。
//
// 构建：
//   cmake --build build --target bench_compare_kernel
// 运行：
//   ./build/bin/bench_compare_kernel [--inner N] [--samples N] [--warmup N] [--verify-only]

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "../src/simd/kernels.h"
#include "../src/simd/selection_mask.h"
#include "../src/type.h"
#include "bench_common.h"

#if defined(__x86_64__) || defined(__i386__) || defined(_M_X64) || defined(_M_IX86)
#include <immintrin.h>
#define BENCH_HAS_X86 1
#else
#define BENCH_HAS_X86 0
#endif

using namespace simple_olap;
using namespace simple_olap::simd;
using namespace simple_olap::bench;

namespace {

constexpr uint32_t kN = 1024;

struct Args {
    uint64_t inner = 20000;
    uint32_t samples = 15;
    uint32_t warmup = 3;
    bool verify_only = false;
};

Args ParseArgs(int argc, char** argv) {
    Args a;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        const auto next = [&]() -> std::string {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "missing value for %s\n", arg.c_str());
                std::exit(1);
            }
            return argv[++i];
        };
        if (arg == "--inner" || arg == "--iters") {
            a.inner = std::stoull(next());
        } else if (arg == "--samples") {
            a.samples = static_cast<uint32_t>(std::stoul(next()));
        } else if (arg == "--warmup") {
            a.warmup = static_cast<uint32_t>(std::stoul(next()));
        } else if (arg == "--verify-only") {
            a.verify_only = true;
        } else {
            std::fprintf(stderr, "unknown argument: %s\n", arg.c_str());
            std::exit(1);
        }
    }
    return a;
}

int32_t g_i32[kN];
int64_t g_i64[kN];
float g_f32[kN];
double g_f64[kN];
SelectionMask g_mask;

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
#endif // BENCH_HAS_X86

bool MaskEqualRaw(const SelectionMask& a, const SelectionMask& b) {
    if (a.row_count() != b.row_count()) {
        return false;
    }
    for (uint32_t w = 0; w < SelectionMask::kWordCount; ++w) {
        if (a.data()[w] != b.data()[w]) {
            return false;
        }
    }
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
    std::printf("backend = %s\n", have_avx2 ? "AVX2" : "SCALAR");
    PrintEnvironment(QueryEnvironment());
    std::printf("inner = %llu, warmup = %u, samples = %u\n\n", static_cast<unsigned long long>(args.inner),
                args.warmup, args.samples);

    CompareKernels scalar;
    InstallScalarCompareKernels(scalar);
    CompareKernels avx2;
    InstallScalarCompareKernels(avx2);
    InstallAvx2CompareKernels(avx2);

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
            if (!MaskEqualRaw(sm, am)) {
                std::printf("VERIFY FAILED: i32 %s count=%u\n", OpName(op), count);
                return 1;
            }
#if BENCH_HAS_X86
            if (have_avx2) {
                OldI32Const(g_i32, count, op, 1234, om);
                if (!MaskEqualRaw(om, am)) {
                    std::printf("VERIFY FAILED: old i32 %s count=%u\n", OpName(op), count);
                    return 1;
                }
            }
#endif

            scalar.f64_const(g_f64, count, op, 0.0, sm);
            avx2.f64_const(g_f64, count, op, 0.0, am);
            if (!MaskEqualRaw(sm, am)) {
                std::printf("VERIFY FAILED: f64 %s count=%u\n", OpName(op), count);
                return 1;
            }
#if BENCH_HAS_X86
            if (have_avx2) {
                OldF64Const(g_f64, count, op, 0.0, om);
                if (!MaskEqualRaw(om, am)) {
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

    std::printf("%6s %-6s %24s %24s %24s %10s %10s\n", "count", "type", "scalar(ns,median)", "old(ns,median)",
                "new(ns,median)", "scalar->avx2", "old->new");
    std::printf("%s\n", std::string(112, '-').c_str());

    for (uint32_t count : counts) {
        struct Row {
            const char* type;
            std::function<void()> scalar_fn;
            std::function<void()> avx2_fn;
            std::function<void()> old_fn; // 可能为空
        };

#if BENCH_HAS_X86
        const bool have_old = have_avx2;
#else
        const bool have_old = false;
#endif

        const Row rows[] = {
            {"i32",
             [&] { scalar.i32_const(g_i32, count, CmpOp::GT, 1234, g_mask); },
             [&] { avx2.i32_const(g_i32, count, CmpOp::GT, 1234, g_mask); },
#if BENCH_HAS_X86
             have_old ? std::function<void()>([&] { OldI32Const(g_i32, count, CmpOp::GT, 1234, g_mask); })
                      : std::function<void()>()
#else
             std::function<void()>()
#endif
            },
            {"i64",
             [&] { scalar.i64_const(g_i64, count, CmpOp::GT, 9007199254740992LL, g_mask); },
             [&] { avx2.i64_const(g_i64, count, CmpOp::GT, 9007199254740992LL, g_mask); },
             std::function<void()>()},
            {"f32",
             [&] { scalar.f32_const(g_f32, count, CmpOp::GT, 0.0f, g_mask); },
             [&] { avx2.f32_const(g_f32, count, CmpOp::GT, 0.0f, g_mask); },
             std::function<void()>()},
            {"f64",
             [&] { scalar.f64_const(g_f64, count, CmpOp::GT, 0.0, g_mask); },
             [&] { avx2.f64_const(g_f64, count, CmpOp::GT, 0.0, g_mask); },
#if BENCH_HAS_X86
             have_old ? std::function<void()>([&] { OldF64Const(g_f64, count, CmpOp::GT, 0.0, g_mask); })
                      : std::function<void()>()
#else
             std::function<void()>()
#endif
            },
        };

        for (const Row& row : rows) {
            std::vector<MicroKernel> kernels;
            // Count() 完全移出 timed region：只在全部 sample 结束后做一次。
            kernels.push_back({"scalar", [&, fn = row.scalar_fn](uint64_t inner) {
                                   for (uint64_t i = 0; i < inner; ++i) {
                                       fn();
                                   }
                                   DoNotOptimize(g_mask.data()[0]);
                               }});
            kernels.push_back({"avx2", [&, fn = row.avx2_fn](uint64_t inner) {
                                   for (uint64_t i = 0; i < inner; ++i) {
                                       fn();
                                   }
                                   DoNotOptimize(g_mask.data()[0]);
                               }});
            if (row.old_fn) {
                kernels.push_back({"old", [&, fn = row.old_fn](uint64_t inner) {
                                       for (uint64_t i = 0; i < inner; ++i) {
                                           fn();
                                       }
                                       DoNotOptimize(g_mask.data()[0]);
                                   }});
            }

            const std::vector<BenchmarkStats> stats =
                RunInterleavedMicroBenchmark(kernels, args.warmup, args.samples, args.inner);

            const BenchmarkStats& scalar_stats = stats[0];
            const BenchmarkStats& avx2_stats = stats[1];
            const BenchmarkStats* old_stats = row.old_fn ? &stats[2] : nullptr;

            const double scalar_speedup =
                avx2_stats.median_ns > 0.0 ? scalar_stats.median_ns / avx2_stats.median_ns : 0.0;
            const double optimization_speedup =
                (old_stats != nullptr && avx2_stats.median_ns > 0.0) ? old_stats->median_ns / avx2_stats.median_ns
                                                                     : 0.0;

            if (old_stats != nullptr) {
                std::printf("%6u %-6s %24.1f %24.1f %24.1f %9.2fx %9.2fx\n", count, row.type,
                            scalar_stats.median_ns, old_stats->median_ns, avx2_stats.median_ns, scalar_speedup,
                            optimization_speedup);
            } else {
                std::printf("%6u %-6s %24.1f %24s %24.1f %9.2fx %10s\n", count, row.type, scalar_stats.median_ns, "-",
                            avx2_stats.median_ns, scalar_speedup, "-");
            }
        }
    }

    return 0;
}
