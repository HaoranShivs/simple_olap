// ============================================================
// Projection 算术内核微基准
// ============================================================
//
// 目的：把「Projection 整列算术」这条路径单独拎出来度量，排除扫描 / 输出
// 物化 / 文件 IO 的干扰（这些在整条 SQL 里占大头，会掩盖算子本身的收益）。
//
// 对比三种实现，对同一批 dense 数据、同一运算：
//
//   [legacy]  旧 ProjectionOperator::ProduceMaterializedProjection 的内层循环：
//             逐行 ReadExecValue -> variant -> ExecValueAsNumber(long double)
//             -> 运算 -> CastNumber -> WriteExecValue。
//   [scalar]  simd::ArithmeticKernels 的 scalar backend（typed 循环）。
//   [avx2]    simd::ArithmeticKernels 的 AVX2 backend。
//
// 计时使用 bench_common.h 的统一 harness：
//   - 每个 sample 至少运行 tens of ms（inner 次 kernel 调用）
//   - 3 轮 warmup + 15 个 sample
//   - 每轮以固定种子随机顺序执行三个 kernel（消除固定 A/B 顺序偏差）
//   - 主指标 median，附 MAD / min / max
//
// 数据生成：mt19937_64 + uniform_real_distribution<double>(-0.5, 0.5)。
// （旧实现用 double 做「整数 PRNG」且不做溢出取模，几十轮后就会变成 inf，
//   已修复。）
//
// 构建：
//   cmake --build build --target bench_arith_kernel
// 运行：
//   ./build/bin/bench_arith_kernel [--count N] [--inner N] [--samples N] [--warmup N] [--verify-only]

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <random>
#include <string>
#include <variant>
#include <vector>

#include "../src/execution/expression/exec_expression.h"
#include "../src/simd/arithmetic_kernel.h"
#include "../src/simd/kernels.h"
#include "bench_common.h"

using namespace simple_olap;
using namespace simple_olap::simd;
using namespace simple_olap::bench;

namespace {

struct Args {
    uint32_t count = kVectorBatchSize; // 一个 batch 的行数
    uint64_t inner = 20000;            // 每个 sample 的 kernel 调用次数
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
        if (arg == "--count") {
            a.count = static_cast<uint32_t>(std::stoul(next()));
        } else if (arg == "--inner") {
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

std::vector<double> MakeF64(uint32_t n) {
    std::mt19937_64 rng(20260919ULL);
    std::uniform_real_distribution<double> dist(-0.5, 0.5);
    std::vector<double> v(n);
    for (uint32_t i = 0; i < n; ++i) {
        v[i] = dist(rng);
    }
    return v;
}

std::vector<int64_t> MakeI64(uint32_t n) {
    std::vector<int64_t> v(n);
    for (uint32_t i = 0; i < n; ++i) {
        v[i] = static_cast<int64_t>(i) * 3 - 17;
    }
    return v;
}

// 旧 Projection 内层循环的忠实复刻（DOUBLE 列 OP DOUBLE 常量）。
void LegacyF64Const(const double* data, double constant, ArithmeticOp op, double* out, uint32_t count) {
    for (uint32_t i = 0; i < count; ++i) {
        const ExecValue lhs = data[i];
        const long double a = ExecValueAsNumber(lhs);
        const ExecValue rhs = constant;
        const long double b = ExecValueAsNumber(rhs);
        const long double r = (op == ArithmeticOp::ADD) ? (a + b) : (a - b);
        out[i] = std::get<double>(CastNumber(r, DataType::DOUBLE));
    }
}

// 旧 Projection 内层循环的忠实复刻（DOUBLE 列 OP DOUBLE 列）。
void LegacyF64Column(const double* lhs, const double* rhs, ArithmeticOp op, double* out, uint32_t count) {
    for (uint32_t i = 0; i < count; ++i) {
        const long double a = ExecValueAsNumber(ExecValue(lhs[i]));
        const long double b = ExecValueAsNumber(ExecValue(rhs[i]));
        const long double r = (op == ArithmeticOp::ADD) ? (a + b) : (a - b);
        out[i] = std::get<double>(CastNumber(r, DataType::DOUBLE));
    }
}

void PrintStats(const char* label, const BenchmarkStats& stats, const BenchmarkStats* reference) {
    std::printf("  %-8s median=%8.3f ns/elem  mad=%7.3f  min=%8.3f  max=%8.3f  mean=%8.3f", label,
                stats.median_ns, stats.mad_ns, stats.min_ns, stats.max_ns, stats.mean_ns);
    if (reference != nullptr && stats.median_ns > 0.0) {
        std::printf("  | speedup vs legacy: %.2fx", reference->median_ns / stats.median_ns);
    }
    std::printf("\n");
}

// 一个场景：legacy / scalar / avx2 三个 kernel 交错计时。
void RunScenario(const char* label, const std::vector<MicroKernel>& kernels, const Args& args) {
    const std::vector<BenchmarkStats> stats =
        RunInterleavedMicroBenchmark(kernels, args.warmup, args.samples, args.inner);

    std::printf("  %s  (count=%u, inner=%llu, samples=%u)\n", label, args.count,
                static_cast<unsigned long long>(args.inner), args.samples);
    PrintStats("legacy", stats[0], nullptr);
    PrintStats("scalar", stats[1], &stats[0]);
    PrintStats("avx2", stats[2], &stats[0]);
    std::printf("\n");
}

} // namespace

int main(int argc, char** argv) {
    const Args args = ParseArgs(argc, argv);

    ArithmeticKernels scalar_kernels;
    ArithmeticKernels avx2_kernels;
    InstallScalarArithmeticKernels(scalar_kernels);
    InstallAvx2ArithmeticKernels(avx2_kernels);

    const bool avx2_available = KernelRegistry::Instance().backend() == SimdBackend::AVX2;

    const auto f64_lhs = MakeF64(args.count);
    const auto f64_rhs = MakeF64(args.count);
    const auto i64_lhs = MakeI64(args.count);

    std::vector<double> out_legacy(args.count);
    std::vector<double> out_scalar(args.count);
    std::vector<double> out_avx2(args.count);
    std::vector<int64_t> iout_scalar(args.count);
    std::vector<int64_t> iout_avx2(args.count);

    constexpr double kConst = 0.25;

    std::printf("=== Projection 算术内核微基准 ===\n");
    PrintEnvironment(QueryEnvironment());
    std::printf("cpu: AVX2 %s\n\n", avx2_available ? "available" : "NOT available (avx2 table == scalar)");

    // ---------- 正确性：AVX2 必须与 scalar 逐字节一致 ----------
    LegacyF64Const(f64_lhs.data(), kConst, ArithmeticOp::ADD, out_legacy.data(), args.count);
    scalar_kernels.f64_const(ArithmeticOp::ADD, f64_lhs.data(), kConst, false, out_scalar.data(), args.count);
    avx2_kernels.f64_const(ArithmeticOp::ADD, f64_lhs.data(), kConst, false, out_avx2.data(), args.count);
    bool ok = std::memcmp(out_scalar.data(), out_avx2.data(), sizeof(double) * args.count) == 0;
    // legacy 走 long double 再落回 double，正常情况下也应一致
    const bool legacy_ok = std::memcmp(out_legacy.data(), out_avx2.data(), sizeof(double) * args.count) == 0;

    scalar_kernels.f64_column(ArithmeticOp::SUB, f64_lhs.data(), f64_rhs.data(), out_scalar.data(), args.count);
    avx2_kernels.f64_column(ArithmeticOp::SUB, f64_lhs.data(), f64_rhs.data(), out_avx2.data(), args.count);
    ok = ok && std::memcmp(out_scalar.data(), out_avx2.data(), sizeof(double) * args.count) == 0;

    scalar_kernels.i64_const(ArithmeticOp::ADD, i64_lhs.data(), int64_t{1}, false, iout_scalar.data(), args.count);
    avx2_kernels.i64_const(ArithmeticOp::ADD, i64_lhs.data(), int64_t{1}, false, iout_avx2.data(), args.count);
    ok = ok && std::memcmp(iout_scalar.data(), iout_avx2.data(), sizeof(int64_t) * args.count) == 0;

    // 尾巴长度（非向量宽度整数倍）
    for (uint32_t tail = 1; tail <= 9; ++tail) {
        const uint32_t n = args.count - tail;
        avx2_kernels.f64_const(ArithmeticOp::SUB, f64_lhs.data(), kConst, true, out_avx2.data(), n);
        scalar_kernels.f64_const(ArithmeticOp::SUB, f64_lhs.data(), kConst, true, out_scalar.data(), n);
        ok = ok && std::memcmp(out_scalar.data(), out_avx2.data(), sizeof(double) * n) == 0;
    }

    std::printf("correctness: scalar == avx2 [%s], legacy == avx2 [%s]\n\n", ok ? "OK" : "FAIL",
                legacy_ok ? "OK" : "DIFF (long-double double-rounding)");
    if (!ok) {
        return 1;
    }
    if (args.verify_only) {
        return 0;
    }

    // ---------- 场景 1：DOUBLE column + constant (ADD) ----------
    RunScenario("DOUBLE column + constant (ADD)",
                {
                    {"legacy", [&](uint64_t inner) {
                         for (uint64_t i = 0; i < inner; ++i) {
                             LegacyF64Const(f64_lhs.data(), kConst, ArithmeticOp::ADD, out_legacy.data(), args.count);
                         }
                         DoNotOptimize(out_legacy[0]);
                     }},
                    {"scalar", [&](uint64_t inner) {
                         for (uint64_t i = 0; i < inner; ++i) {
                             scalar_kernels.f64_const(ArithmeticOp::ADD, f64_lhs.data(), kConst, false,
                                                      out_scalar.data(), args.count);
                         }
                         DoNotOptimize(out_scalar[0]);
                     }},
                    {"avx2", [&](uint64_t inner) {
                         for (uint64_t i = 0; i < inner; ++i) {
                             avx2_kernels.f64_const(ArithmeticOp::ADD, f64_lhs.data(), kConst, false,
                                                    out_avx2.data(), args.count);
                         }
                         DoNotOptimize(out_avx2[0]);
                     }},
                },
                args);

    // ---------- 场景 2：DOUBLE constant - column (SUB, const-on-left) ----------
    RunScenario("DOUBLE constant - column (SUB, const-on-left)",
                {
                    {"legacy", [&](uint64_t inner) {
                         for (uint64_t i = 0; i < inner; ++i) {
                             LegacyF64Const(f64_lhs.data(), kConst, ArithmeticOp::SUB, out_legacy.data(), args.count);
                         }
                         DoNotOptimize(out_legacy[0]);
                     }},
                    {"scalar", [&](uint64_t inner) {
                         for (uint64_t i = 0; i < inner; ++i) {
                             scalar_kernels.f64_const(ArithmeticOp::SUB, f64_lhs.data(), kConst, true,
                                                      out_scalar.data(), args.count);
                         }
                         DoNotOptimize(out_scalar[0]);
                     }},
                    {"avx2", [&](uint64_t inner) {
                         for (uint64_t i = 0; i < inner; ++i) {
                             avx2_kernels.f64_const(ArithmeticOp::SUB, f64_lhs.data(), kConst, true,
                                                    out_avx2.data(), args.count);
                         }
                         DoNotOptimize(out_avx2[0]);
                     }},
                },
                args);

    // ---------- 场景 3：DOUBLE column - column (SUB) ----------
    RunScenario("DOUBLE column - column (SUB)",
                {
                    {"legacy", [&](uint64_t inner) {
                         for (uint64_t i = 0; i < inner; ++i) {
                             LegacyF64Column(f64_lhs.data(), f64_rhs.data(), ArithmeticOp::SUB, out_legacy.data(),
                                             args.count);
                         }
                         DoNotOptimize(out_legacy[0]);
                     }},
                    {"scalar", [&](uint64_t inner) {
                         for (uint64_t i = 0; i < inner; ++i) {
                             scalar_kernels.f64_column(ArithmeticOp::SUB, f64_lhs.data(), f64_rhs.data(),
                                                       out_scalar.data(), args.count);
                         }
                         DoNotOptimize(out_scalar[0]);
                     }},
                    {"avx2", [&](uint64_t inner) {
                         for (uint64_t i = 0; i < inner; ++i) {
                             avx2_kernels.f64_column(ArithmeticOp::SUB, f64_lhs.data(), f64_rhs.data(),
                                                     out_avx2.data(), args.count);
                         }
                         DoNotOptimize(out_avx2[0]);
                     }},
                },
                args);

    // ---------- INT64：legacy 无对应逐行复刻（旧路径也是 long double），只看 scalar vs avx2 ----------
    {
        const std::vector<BenchmarkStats> stats = RunInterleavedMicroBenchmark(
            {
                {"scalar", [&](uint64_t inner) {
                     for (uint64_t i = 0; i < inner; ++i) {
                         scalar_kernels.i64_const(ArithmeticOp::ADD, i64_lhs.data(), int64_t{1}, false,
                                                  iout_scalar.data(), args.count);
                     }
                     DoNotOptimize(iout_scalar[0]);
                 }},
                {"avx2", [&](uint64_t inner) {
                     for (uint64_t i = 0; i < inner; ++i) {
                         avx2_kernels.i64_const(ArithmeticOp::ADD, i64_lhs.data(), int64_t{1}, false,
                                                iout_avx2.data(), args.count);
                     }
                     DoNotOptimize(iout_avx2[0]);
                 }},
            },
            args.warmup, args.samples, args.inner);

        std::printf("  INT64 column + constant (ADD)  (count=%u, inner=%llu, samples=%u)\n", args.count,
                    static_cast<unsigned long long>(args.inner), args.samples);
        PrintStats("scalar", stats[0], nullptr);
        PrintStats("avx2", stats[1], nullptr);
    }

    return 0;
}
