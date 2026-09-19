// ============================================================
// Selection Pipeline 微基准：mask-native vs selection-vector 往返
// ============================================================
//
// 场景：Storage 下推谓词产生 mask1，执行层 Filter 产生 mask2，
// 最终需要 mask1 AND mask2 的结果。
//
//   [old]  mask1 -> ToSelectionVector -> FromSelectionVector -> AND -> ToSelectionVector
//   [new]  mask1 AND mask2（直接位运算）
//
// 另测「遍历 active rows」的两种方式：
//   [old]  先 ToSelectionVector，再按 vector<uint32_t> 迭代
//   [new]  SelectionMask::ForEachSetBit（无中间数组）
//
// 统一 harness：warmup + samples + median/MAD + 固定种子随机交错，
// 避免「old 总在 new 前」造成的频率/温度偏差。
//
// 构建：
//   cmake --build build --target bench_selection_pipeline
// 运行：
//   ./build/bin/bench_selection_pipeline [--count N] [--inner N] [--samples N] [--warmup N]

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "../src/simd/selection_mask.h"
#include "bench_common.h"

using namespace simple_olap::simd;
using namespace simple_olap::bench;

namespace {

void BuildMask(SelectionMask& mask, uint32_t n, uint32_t percent) {
    mask.SetNone(n);
    for (uint32_t row = 0; row < n; ++row) {
        if ((row * 2654435761u) % 100u < percent) {
            mask.data()[row >> 6] |= (uint64_t(1) << (row & 63));
        }
    }
}

} // namespace

int main(int argc, char** argv) {
    uint32_t n = simple_olap::kVectorBatchSize;
    uint64_t inner = 20000;
    uint32_t samples = 15;
    uint32_t warmup = 3;

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
            n = static_cast<uint32_t>(std::stoul(next()));
        } else if (arg == "--inner" || arg == "--iters") {
            inner = std::stoull(next());
        } else if (arg == "--samples") {
            samples = static_cast<uint32_t>(std::stoul(next()));
        } else if (arg == "--warmup") {
            warmup = static_cast<uint32_t>(std::stoul(next()));
        } else {
            std::fprintf(stderr, "unknown argument: %s\n", arg.c_str());
            std::exit(1);
        }
    }

    std::printf("count = %u\n", n);
    PrintEnvironment(QueryEnvironment());
    std::printf("inner = %llu, warmup = %u, samples = %u\n\n", static_cast<unsigned long long>(inner), warmup, samples);
    std::printf("%8s %8s | %26s | %26s\n", "percent", "active", "AND roundtrip vs native (ns)", "scan ns/row");
    std::printf("%8s %8s | %12s %12s | %12s %12s\n", "", "", "old", "new", "old", "new");
    std::printf("%s\n", std::string(96, '-').c_str());

    std::vector<uint32_t> sel(n); // 复用缓冲，模拟旧路径的 sel_vector

    for (uint32_t percent : {1u, 10u, 25u, 50u, 75u, 90u, 100u}) {
        SelectionMask mask1;
        SelectionMask mask2;
        BuildMask(mask1, n, percent);
        BuildMask(mask2, n, percent > 50 ? percent - 10 : percent + 10);

        const std::vector<MicroKernel> kernels = {
            {"old-and",
             [&](uint64_t iterations) {
                 for (uint64_t i = 0; i < iterations; ++i) {
                     mask1.ToSelectionVector(sel);
                     SelectionMask from;
                     from.FromSelectionVector(sel, n);
                     from.And(mask2);
                     from.ToSelectionVector(sel);
                 }
                 DoNotOptimize(sel.data()[0]);
             }},
            {"new-and",
             [&](uint64_t iterations) {
                 uint64_t acc = 0;
                 for (uint64_t i = 0; i < iterations; ++i) {
                     SelectionMask result = mask1;
                     result.And(mask2);
                     acc += result.Count();
                 }
                 DoNotOptimize(acc);
             }},
            {"old-scan",
             [&](uint64_t iterations) {
                 uint64_t acc = 0;
                 for (uint64_t i = 0; i < iterations; ++i) {
                     mask1.ToSelectionVector(sel);
                     for (uint32_t row : sel) {
                         acc += row;
                     }
                 }
                 DoNotOptimize(acc);
             }},
            {"new-scan",
             [&](uint64_t iterations) {
                 uint64_t acc = 0;
                 for (uint64_t i = 0; i < iterations; ++i) {
                     mask1.ForEachSetBit([&](uint32_t row) { acc += row; });
                 }
                 DoNotOptimize(acc);
             }},
        };

        const std::vector<BenchmarkStats> stats = RunInterleavedMicroBenchmark(kernels, warmup, samples, inner);

        const uint32_t active = mask1.Count();
        const double rows = active > 0 ? static_cast<double>(active) : 1.0;
        std::printf("%7u%% %8u | %12.1f %12.1f | %12.3f %12.3f\n", percent, active, stats[0].median_ns,
                    stats[1].median_ns, stats[2].median_ns / rows, stats[3].median_ns / rows);
    }

    return 0;
}
