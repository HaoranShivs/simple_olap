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
// 构建：
//   cmake --build build --target bench_selection_pipeline
// 运行：
//   ./build/bin/bench_selection_pipeline [--iters N] [--count N]

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "../src/simd/selection_mask.h"

using namespace simple_olap::simd;

namespace {

using Clock = std::chrono::steady_clock;

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

} // namespace

int main(int argc, char** argv) {
    uint32_t n = simple_olap::kVectorBatchSize;
    uint32_t iters = 200000;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--count" && i + 1 < argc) {
            n = static_cast<uint32_t>(std::stoul(argv[++i]));
        } else if (arg == "--iters" && i + 1 < argc) {
            iters = static_cast<uint32_t>(std::stoul(argv[++i]));
        }
    }

    std::printf("count = %u, iters = %u\n\n", n, iters);
    std::printf("%8s %10s %16s %16s %14s %14s\n", "percent", "active", "old-roundtrip(ns)", "new-and(ns)",
                "old-scan(ns/row)", "new-scan(ns/row)");
    std::printf("%s\n", std::string(86, '-').c_str());

    std::vector<uint32_t> sel(n); // 复用缓冲，模拟旧路径的 sel_vector
    volatile uint64_t sink = 0;

    for (uint32_t percent : {1u, 10u, 25u, 50u, 75u, 90u, 100u}) {
        SelectionMask mask1;
        SelectionMask mask2;
        BuildMask(mask1, n, percent);
        BuildMask(mask2, n, percent > 50 ? percent - 10 : percent + 10);

        // ---------- AND：旧路径（两次往返）vs 新路径（直接位运算） ----------
        const double old_ns = TimeNs(
            [&]() {
                mask1.ToSelectionVector(sel);
                SelectionMask from;
                from.FromSelectionVector(sel, n);
                from.And(mask2);
                from.ToSelectionVector(sel);
                sink += sel.size();
            },
            iters);

        const double new_ns = TimeNs(
            [&]() {
                SelectionMask result = mask1;
                result.And(mask2);
                sink += result.Count();
            },
            iters);

        // ---------- 遍历 active rows ----------
        const double old_scan_ns = TimeNs(
            [&]() {
                mask1.ToSelectionVector(sel);
                uint64_t acc = 0;
                for (uint32_t row : sel) {
                    acc += row;
                }
                sink += acc;
            },
            iters);

        const double new_scan_ns = TimeNs(
            [&]() {
                uint64_t acc = 0;
                mask1.ForEachSetBit([&](uint32_t row) { acc += row; });
                sink += acc;
            },
            iters);

        const uint32_t active = mask1.Count();
        const double rows = active > 0 ? static_cast<double>(active) : 1.0;
        std::printf("%7u%% %10u %16.1f %16.1f %14.3f %14.3f\n", percent, active, old_ns, new_ns,
                    old_scan_ns / rows, new_scan_ns / rows);
    }

    (void)sink;
    return 0;
}
