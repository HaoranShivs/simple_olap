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
// 内核直接来自 InstallScalarArithmeticKernels / InstallAvx2ArithmeticKernels，
// 因此不依赖运行时 CPU 探测，可在一台机器上公平对比两个后端。
//
// 构建：
//   cmake --build build --target bench_arith_kernel
// 运行：
//   ./build/bin/bench_arith_kernel [--iters N] [--count N] [--verify-only]
//
// 若 CPU 无 AVX2，AVX2 表会与 scalar 相同（本程序会提示）。

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <variant>
#include <vector>

#include "../src/execution/expression/exec_expression.h"
#include "../src/simd/arithmetic_kernel.h"
#include "../src/simd/kernels.h"

using namespace simple_olap;
using namespace simple_olap::simd;

namespace {

using Clock = std::chrono::steady_clock;

double NowNs() {
    return static_cast<double>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count());
}

struct Args {
    uint32_t count = kVectorBatchSize; // 一个 batch 的行数
    uint32_t iters = 200000;           // 迭代轮数
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
        } else {
            std::fprintf(stderr, "unknown argument: %s\n", arg.c_str());
            std::exit(1);
        }
    }
    return a;
}

std::vector<double> MakeF64(uint32_t n, double seed) {
    std::vector<double> v(n);
    double x = seed;
    for (uint32_t i = 0; i < n; ++i) {
        x = x * 1103515245.0 + 12345.0;
        v[i] = (x / 2147483648.0) - 0.5; // 大约 [-0.5, 0.5)
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

template <typename F> double TimeNs(uint32_t iters, F&& fn) {
    const double start = NowNs();
    for (uint32_t it = 0; it < iters; ++it) {
        fn();
    }
    return NowNs() - start;
}

struct Result {
    const char* name;
    double ns_per_elem;
};

// 注意：调用方传入的 ns_per_elem 是 TimeNs(iters, fn) / count，即「摊到每行」的
// 累计时间；再除以 iters 才是真正的单元素耗时。这里统一归一化后再打印。
void Report(const char* label, const Result& legacy, const Result& scalar, const Result& avx2, uint32_t iters,
            uint32_t count) {
    const double per_iter = static_cast<double>(iters);
    const double elems = per_iter * static_cast<double>(count);
    const double legacy_ns = legacy.ns_per_elem / per_iter;
    const double scalar_ns = scalar.ns_per_elem / per_iter;
    const double avx2_ns = avx2.ns_per_elem / per_iter;
    std::printf("  %s  (count=%u, iters=%u, %.1f M elems)\n", label, count, iters, elems / 1e6);
    std::printf("    legacy: %8.3f ns/elem\n", legacy_ns);
    std::printf("    scalar: %8.3f ns/elem   speedup vs legacy: %.2fx\n", scalar_ns, legacy_ns / scalar_ns);
    std::printf("    avx2  : %8.3f ns/elem   speedup vs legacy: %.2fx | vs scalar: %.2fx\n\n", avx2_ns,
                legacy_ns / avx2_ns, scalar_ns / avx2_ns);
}

} // namespace

int main(int argc, char** argv) {
    const Args args = ParseArgs(argc, argv);

    ArithmeticKernels scalar_kernels;
    ArithmeticKernels avx2_kernels;
    InstallScalarArithmeticKernels(scalar_kernels);
    InstallAvx2ArithmeticKernels(avx2_kernels);

    const bool avx2_available = KernelRegistry::Instance().backend() == SimdBackend::AVX2;

    const auto f64_lhs = MakeF64(args.count, 1.0);
    const auto f64_rhs = MakeF64(args.count, 7.0);
    const auto i64_lhs = MakeI64(args.count);

    std::vector<double> out_legacy(args.count);
    std::vector<double> out_scalar(args.count);
    std::vector<double> out_avx2(args.count);
    std::vector<int64_t> iout_scalar(args.count);
    std::vector<int64_t> iout_avx2(args.count);

    constexpr double kConst = 0.25;

    std::printf("=== Projection 算术内核微基准 ===\n");
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

    // ---------- 计时 ----------
    Report("DOUBLE column + constant (ADD)",
           {"legacy",
            TimeNs(args.iters,
                   [&] { LegacyF64Const(f64_lhs.data(), kConst, ArithmeticOp::ADD, out_legacy.data(), args.count); }) /
                args.count},
           {"scalar", TimeNs(args.iters,
                             [&] {
                                 scalar_kernels.f64_const(ArithmeticOp::ADD, f64_lhs.data(), kConst, false,
                                                          out_scalar.data(), args.count);
                             }) /
                          args.count},
           {"avx2", TimeNs(args.iters,
                           [&] {
                               avx2_kernels.f64_const(ArithmeticOp::ADD, f64_lhs.data(), kConst, false, out_avx2.data(),
                                                      args.count);
                           }) /
                        args.count},
           args.iters, args.count);

    Report("DOUBLE constant - column (SUB, const-on-left)",
           {"legacy",
            TimeNs(args.iters,
                   [&] { LegacyF64Const(f64_lhs.data(), kConst, ArithmeticOp::SUB, out_legacy.data(), args.count); }) /
                args.count},
           {"scalar", TimeNs(args.iters,
                             [&] {
                                 scalar_kernels.f64_const(ArithmeticOp::SUB, f64_lhs.data(), kConst, true,
                                                          out_scalar.data(), args.count);
                             }) /
                          args.count},
           {"avx2", TimeNs(args.iters,
                           [&] {
                               avx2_kernels.f64_const(ArithmeticOp::SUB, f64_lhs.data(), kConst, true, out_avx2.data(),
                                                      args.count);
                           }) /
                        args.count},
           args.iters, args.count);

    Report("DOUBLE column - column (SUB)",
           {"legacy", TimeNs(args.iters,
                             [&] {
                                 LegacyF64Column(f64_lhs.data(), f64_rhs.data(), ArithmeticOp::SUB, out_legacy.data(),
                                                 args.count);
                             }) /
                          args.count},
           {"scalar", TimeNs(args.iters,
                             [&] {
                                 scalar_kernels.f64_column(ArithmeticOp::SUB, f64_lhs.data(), f64_rhs.data(),
                                                           out_scalar.data(), args.count);
                             }) /
                          args.count},
           {"avx2", TimeNs(args.iters,
                           [&] {
                               avx2_kernels.f64_column(ArithmeticOp::SUB, f64_lhs.data(), f64_rhs.data(),
                                                       out_avx2.data(), args.count);
                           }) /
                        args.count},
           args.iters, args.count);

    // INT64：legacy 无对应逐行复刻（旧路径也是 long double），只看 scalar vs avx2
    {
        const double scalar_ns = TimeNs(args.iters,
                                        [&] {
                                            scalar_kernels.i64_const(ArithmeticOp::ADD, i64_lhs.data(), int64_t{1},
                                                                     false, iout_scalar.data(), args.count);
                                        }) /
                                 args.count;
        const double avx2_ns = TimeNs(args.iters,
                                      [&] {
                                          avx2_kernels.i64_const(ArithmeticOp::ADD, i64_lhs.data(), int64_t{1}, false,
                                                                 iout_avx2.data(), args.count);
                                      }) /
                               args.count;
        const double per_iter = static_cast<double>(args.iters);
        std::printf("  INT64 column + constant (ADD)  (count=%u, iters=%u)\n", args.count, args.iters);
        std::printf("    scalar: %8.3f ns/elem\n", scalar_ns / per_iter);
        std::printf("    avx2  : %8.3f ns/elem   vs scalar: %.2fx\n\n", avx2_ns / per_iter, scalar_ns / avx2_ns);
    }

    return 0;
}
