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
// 采样：ns / active row、ns / physical row、active rows/s。
//
// 构建：
//   cmake --build build --target bench_sparse_projection
// 运行：
//   ./build/bin/bench_sparse_projection [--iters N] [--count N] [--verify-only]

#include <algorithm>
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

using namespace simple_olap;
using namespace simple_olap::simd;

namespace {

using Clock = std::chrono::steady_clock;

struct Args {
    uint32_t count = kVectorBatchSize;
    uint32_t iters = 200000;
    bool verify_only = false;
};

Args ParseArgs(int argc, char** argv) {
    Args a;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--count" && i + 1 < argc) {
            a.count = static_cast<uint32_t>(std::stoul(argv[++i]));
        } else if (arg == "--iters" && i + 1 < argc) {
            a.iters = static_cast<uint32_t>(std::stoul(argv[++i]));
        } else if (arg == "--verify-only") {
            a.verify_only = true;
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

template <typename Fn> double TimeNs(Fn&& fn, uint32_t iters) {
    const auto t0 = Clock::now();
    for (uint32_t i = 0; i < iters; ++i) {
        fn();
    }
    const auto t1 = Clock::now();
    return static_cast<double>(std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count()) /
           static_cast<double>(iters);
}

// 旧路径：忠实复刻旧 ProjectionOperator::ProduceMaterializedProjection 的内层循环：
// 逐行 expr->Eval（virtual + variant）-> WriteExecValue。
double RunLegacy(const VectorBatch& input, const ExecExpression& expr, ColumnData& output,
                 const SelectionMask& mask) {
    uint32_t out = 0;
    mask.ForEachSetBit([&](uint32_t row) {
        const ExecValue value = expr.Eval(input, row);
        WriteExecValue(output, out, value);
        ++out;
    });

    double acc = 0.0;
    const double* data = output.data<double>();
    for (uint32_t i = 0; i < out; ++i) {
        acc += data[i];
    }
    return acc;
}

// 新路径：mask -> typed gather -> dense typed 运算。
template <typename GatherFn, typename ArithFn>
double RunGather(const double* src, const SelectionMask& mask, double* dense, double* dst, GatherFn gather,
                 ArithFn arith) {
    const uint32_t active = mask.Count();
    gather(src, mask, dense);
    arith(ArithmeticOp::ADD, dense, 1.0, false, dst, active);

    double acc = 0.0;
    for (uint32_t i = 0; i < active; ++i) {
        acc += dst[i];
    }
    return acc;
}

} // namespace

int main(int argc, char** argv) {
    const Args args = ParseArgs(argc, argv);
    const uint32_t n = args.count;

    const KernelRegistry& reg = KernelRegistry::Instance();
    const bool have_avx2 = (reg.backend() == SimdBackend::AVX2);
    std::printf("backend = %s, count = %u, iters = %u\n\n", have_avx2 ? "AVX2" : "SCALAR", n, args.iters);

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

    std::printf("%8s %10s %14s %14s %14s %14s %14s\n", "percent", "active", "legacy(ns/row)", "scalar(ns/row)",
                "avx2(ns/row)", "scalar(ns/phys)", "avx2(ns/physical)");
    std::printf("%s\n", std::string(96, '-').c_str());

    for (uint32_t percent : percents) {
        SelectionMask mask;
        BuildMask(mask, n, percent);
        const uint32_t active = mask.Count();

        // 正确性校验：三条路径结果必须一致。
        const double acc_legacy = RunLegacy(input, expr, legacy_output, mask);
        std::memcpy(dst_legacy.data(), legacy_output.data<double>(), sizeof(double) * active);
        const double acc_scalar =
            RunGather(src.data(), mask, dense.data(), dst_scalar.data(), scalar_gather.f64, scalar_arith.f64_const);
        const double acc_avx2 =
            RunGather(src.data(), mask, dense.data(), dst_avx2.data(), avx2_gather.f64, avx2_arith.f64_const);

        bool ok = std::fabs(acc_legacy - acc_scalar) < 1e-6 && std::fabs(acc_scalar - acc_avx2) < 1e-6;
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

        volatile double sink = 0.0;
        const double legacy_ns = TimeNs(
            [&]() { sink = sink + RunLegacy(input, expr, legacy_output, mask); }, args.iters);
        const double scalar_ns = TimeNs(
            [&]() {
                sink = sink + RunGather(src.data(), mask, dense.data(), dst_scalar.data(), scalar_gather.f64,
                                        scalar_arith.f64_const);
            },
            args.iters);
        const double avx2_ns = TimeNs(
            [&]() {
                sink = sink + RunGather(src.data(), mask, dense.data(), dst_avx2.data(), avx2_gather.f64,
                                        avx2_arith.f64_const);
            },
            args.iters);
        (void)sink;

        const double active_rows = active > 0 ? static_cast<double>(active) : 1.0;
        std::printf("%7u%% %10u %14.2f %14.2f %14.2f %14.2f %14.2f\n", percent, active, legacy_ns / active_rows,
                    scalar_ns / active_rows, avx2_ns / active_rows, scalar_ns / n, avx2_ns / n);
    }

    if (args.verify_only) {
        std::printf("all sparse projection paths agree\n");
    }
    return 0;
}
