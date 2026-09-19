// ============================================================
// Sparse Projection 微基准：mask-native gather vs 旧逐行物化
// ============================================================
//
// 目的：把「Filter 之后的稀疏投影」这条路径单独拎出来度量，排除扫描 /
// 文件 IO 的干扰。
//
// 对比三种实现，对同一批稀疏有效行、同一运算（value + 1）：
//
//   [legacy]  旧 ProjectionOperator 的稀疏路径：
//             selection -> 每行 ReadExecValue -> variant -> long double 运算
//   [scalar]  SelectionMask -> scalar gather -> dense typed 运算
//   [avx2]    SelectionMask -> AVX2 gather -> AVX2 运算
//
// 计时拆成两层（BenchmarkSettingV2 §10）：
//   [kernel]    gather + arithmetic，timed region 内不做输出扫描
//   [pipeline]  gather + arithmetic + 下游 checksum（完整消费成本）
//
// 旧实现把 checksum 扫描留在 timed region，会掩盖 gather/AVX2 的收益；
// 现在 kernel-only 用于评估局部优化，pipeline 用于评估端到端算子成本。
//
// 统一 harness：warmup + samples + median/MAD + 随机交错。
//
// 构建：
//   cmake --build build --target bench_sparse_projection
// 运行：
//   ./build/bin/bench_sparse_projection [--inner N] [--count N] [--samples N] [--warmup N] [--verify-only]

#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "../src/execution/expression/exec_expression.h"
#include "../src/execution/vector/vector.h"
#include "../src/memory/buffer_pool/buffer_pool.h"
#include "../src/simd/gather_kernel.h"
#include "../src/simd/kernels.h"
#include "../src/simd/selection_mask.h"
#include "../src/type.h"
#include "bench_common.h"

using namespace simple_olap;
using namespace simple_olap::simd;
using namespace simple_olap::bench;

namespace {

struct Args {
    uint32_t count = kVectorBatchSize;
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
        if (arg == "--count") {
            a.count = static_cast<uint32_t>(std::stoul(next()));
        } else if (arg == "--inner" || arg == "--iters") {
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

void BuildMask(SelectionMask& mask, uint32_t n, uint32_t percent) {
    mask.SetNone(n);
    for (uint32_t row = 0; row < n; ++row) {
        if ((row * 2654435761u) % 100u < percent) {
            mask.data()[row >> 6] |= (uint64_t(1) << (row & 63));
        }
    }
}

// 旧路径 kernel：逐行 expr->Eval（virtual + variant）-> WriteExecValue。
uint32_t RunLegacyKernel(const VectorBatch& input, const ExecExpression& expr, ColumnData& output,
                         const SelectionMask& mask) {
    uint32_t out = 0;
    mask.ForEachSetBit([&](uint32_t row) {
        const ExecValue value = expr.Eval(input, row);
        WriteExecValue(output, out, value);
        ++out;
    });
    return out;
}

// 下游消费：对输出做一次完整扫描（模拟聚合 / 输出物化）。
double Checksum(const double* data, uint32_t count) {
    double acc = 0.0;
    for (uint32_t i = 0; i < count; ++i) {
        acc += data[i];
    }
    return acc;
}

// 新路径 kernel：mask -> typed gather -> dense typed 运算。
template <typename GatherFn, typename ArithFn>
uint32_t RunGatherKernel(const double* src, const SelectionMask& mask, double* dense, double* dst, GatherFn gather,
                         ArithFn arith) {
    const uint32_t active = mask.Count();
    gather(src, mask, dense);
    arith(ArithmeticOp::ADD, dense, 1.0, false, dst, active);
    return active;
}

} // namespace

int main(int argc, char** argv) {
    const Args args = ParseArgs(argc, argv);
    const uint32_t n = args.count;

    const KernelRegistry& reg = KernelRegistry::Instance();
    const bool have_avx2 = (reg.backend() == SimdBackend::AVX2);
    std::printf("backend = %s, count = %u\n", have_avx2 ? "AVX2" : "SCALAR", n);
    PrintEnvironment(QueryEnvironment());
    std::printf("inner = %llu, warmup = %u, samples = %u\n\n", static_cast<unsigned long long>(args.inner),
                args.warmup, args.samples);

    GatherKernels scalar_gather;
    InstallScalarGatherKernels(scalar_gather);
    GatherKernels avx2_gather;
    InstallScalarGatherKernels(avx2_gather);
    InstallAvx2GatherKernels(avx2_gather);

    ArithmeticKernels scalar_arith;
    InstallScalarArithmeticKernels(scalar_arith);
    ArithmeticKernels avx2_arith;
    InstallScalarArithmeticKernels(avx2_arith);
    InstallAvx2ArithmeticKernels(avx2_arith);

    BufferPool pool;
    VectorBatch input(&pool, /*is_view=*/false);
    input.AddColumn(DataType::DOUBLE);
    input.columns[0].Resize(n);
    std::vector<double> src(n);
    for (uint32_t i = 0; i < n; ++i) {
        src[i] = static_cast<double>(i) * 0.5 - 100.0;
    }
    std::memcpy(input.columns[0].mutable_data<double>(), src.data(), sizeof(double) * n);
    input.SetIdentitySelection(n);

    // value + 1 的逐行 ExecExpression（旧路径使用）。
    ExecBinary expr(PlanBinaryOp::ADD, std::make_unique<ExecColumnRef>(0, DataType::DOUBLE),
                    std::make_unique<ExecLiteral>(ExecValue{double(1.0)}, DataType::DOUBLE), DataType::DOUBLE);
    ColumnData legacy_output(DataType::DOUBLE, &pool);
    legacy_output.Resize(n);

    std::vector<double> dense(n);
    std::vector<double> dst_legacy(n);
    std::vector<double> dst_scalar(n);
    std::vector<double> dst_avx2(n);

    const uint32_t percents[] = {1, 10, 25, 50, 75, 90, 99};

    std::printf("%8s %8s | %22s | %22s\n", "percent", "active", "kernel ns/active-row", "pipeline ns/active-row");
    std::printf("%8s %8s | %8s %8s %8s | %8s %8s %8s\n", "", "", "legacy", "scalar", "avx2", "legacy", "scalar",
                "avx2");
    std::printf("%s\n", std::string(96, '-').c_str());

    for (uint32_t percent : percents) {
        SelectionMask mask;
        BuildMask(mask, n, percent);
        const uint32_t active = mask.Count();

        // 正确性校验：三条路径结果必须一致。
        const uint32_t legacy_count = RunLegacyKernel(input, expr, legacy_output, mask);
        std::memcpy(dst_legacy.data(), legacy_output.data<double>(), sizeof(double) * legacy_count);
        const double acc_legacy = Checksum(dst_legacy.data(), legacy_count);
        const uint32_t scalar_count =
            RunGatherKernel(src.data(), mask, dense.data(), dst_scalar.data(), scalar_gather.f64, scalar_arith.f64_const);
        const uint32_t avx2_count =
            RunGatherKernel(src.data(), mask, dense.data(), dst_avx2.data(), avx2_gather.f64, avx2_arith.f64_const);
        const double acc_scalar = Checksum(dst_scalar.data(), scalar_count);
        const double acc_avx2 = Checksum(dst_avx2.data(), avx2_count);

        bool ok = legacy_count == scalar_count && scalar_count == avx2_count &&
                  std::fabs(acc_legacy - acc_scalar) < 1e-6 && std::fabs(acc_scalar - acc_avx2) < 1e-6;
        for (uint32_t i = 0; i < active && ok; ++i) {
            ok = std::fabs(dst_legacy[i] - dst_scalar[i]) < 1e-12 && std::fabs(dst_scalar[i] - dst_avx2[i]) < 1e-12;
        }
        if (!ok) {
            std::printf("VERIFY FAILED at percent=%u\n", percent);
            return 1;
        }

        if (args.verify_only) {
            continue;
        }

        const std::vector<MicroKernel> kernels = {
            {"legacy-kernel",
             [&](uint64_t inner) {
                 for (uint64_t i = 0; i < inner; ++i) {
                     RunLegacyKernel(input, expr, legacy_output, mask);
                 }
                 DoNotOptimize(legacy_output.data<double>()[0]);
             }},
            {"scalar-kernel",
             [&](uint64_t inner) {
                 for (uint64_t i = 0; i < inner; ++i) {
                     RunGatherKernel(src.data(), mask, dense.data(), dst_scalar.data(), scalar_gather.f64,
                                     scalar_arith.f64_const);
                 }
                 DoNotOptimize(dst_scalar[0]);
             }},
            {"avx2-kernel",
             [&](uint64_t inner) {
                 for (uint64_t i = 0; i < inner; ++i) {
                     RunGatherKernel(src.data(), mask, dense.data(), dst_avx2.data(), avx2_gather.f64,
                                     avx2_arith.f64_const);
                 }
                 DoNotOptimize(dst_avx2[0]);
             }},
            {"legacy-pipeline",
             [&](uint64_t inner) {
                 double acc = 0.0;
                 for (uint64_t i = 0; i < inner; ++i) {
                     const uint32_t rows = RunLegacyKernel(input, expr, legacy_output, mask);
                     acc += Checksum(legacy_output.data<double>(), rows);
                 }
                 DoNotOptimize(acc);
             }},
            {"scalar-pipeline",
             [&](uint64_t inner) {
                 double acc = 0.0;
                 for (uint64_t i = 0; i < inner; ++i) {
                     const uint32_t rows = RunGatherKernel(src.data(), mask, dense.data(), dst_scalar.data(),
                                                           scalar_gather.f64, scalar_arith.f64_const);
                     acc += Checksum(dst_scalar.data(), rows);
                 }
                 DoNotOptimize(acc);
             }},
            {"avx2-pipeline",
             [&](uint64_t inner) {
                 double acc = 0.0;
                 for (uint64_t i = 0; i < inner; ++i) {
                     const uint32_t rows = RunGatherKernel(src.data(), mask, dense.data(), dst_avx2.data(),
                                                             avx2_gather.f64, avx2_arith.f64_const);
                     acc += Checksum(dst_avx2.data(), rows);
                 }
                 DoNotOptimize(acc);
             }},
        };

        const std::vector<BenchmarkStats> stats =
            RunInterleavedMicroBenchmark(kernels, args.warmup, args.samples, args.inner);

        const double active_rows = active > 0 ? static_cast<double>(active) : 1.0;
        std::printf("%7u%% %8u | %8.2f %8.2f %8.2f | %8.2f %8.2f %8.2f\n", percent, active,
                    stats[0].median_ns / active_rows, stats[1].median_ns / active_rows,
                    stats[2].median_ns / active_rows, stats[3].median_ns / active_rows,
                    stats[4].median_ns / active_rows, stats[5].median_ns / active_rows);
    }

    if (args.verify_only) {
        std::printf("all sparse projection paths agree\n");
    }
    return 0;
}
