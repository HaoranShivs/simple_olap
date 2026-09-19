#pragma once

// ============================================================
// Benchmark 公共设施：
//   1. 延迟统计（nearest-rank 百分位 / QPS）
//   2. 微基准 harness（warmup + 多 sample + median/MAD + A/B 交错）
//   3. 内存池增量统计
//   4. 运行环境打印（commit / 编译器 / CPU / AVX2 / batch size）
// ============================================================

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <numeric>
#include <random>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "../src/common/constants.h" // kVectorBatchSize
#include "../src/execution/execution_context.h" // ExecutionMode
#include "../src/memory/block_pool/block_pool.h"
#include "../src/memory/buffer_pool/buffer_pool.h"
#include "../src/memory/query_memory_context/query_memory_context.h" // QueryMemoryMode
#include "../src/type.h"

#ifndef SIMPLE_OLAP_GIT_COMMIT
#define SIMPLE_OLAP_GIT_COMMIT "unknown"
#endif
#ifndef SIMPLE_OLAP_BUILD_TYPE
#define SIMPLE_OLAP_BUILD_TYPE "unknown"
#endif

namespace simple_olap::bench {

using Clock = std::chrono::steady_clock;

inline uint64_t NowNs() {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count());
}

// ==========================================================
// 配置
// ==========================================================

struct BenchOptions {
    std::filesystem::path db_path;

    uint64_t rows = 4'000'000;

    size_t clients = 1;

    // 预热与测量均以「真实墙钟时间」为准。
    uint32_t warmup_seconds = 10;
    uint32_t duration_seconds = 60;
    uint64_t min_samples = 0;

    // 单 Query 内部并行（MULTI_THREAD）与 client 间并发要分开测：
    // QPS 矩阵固定 SINGLE_THREAD，并行扩展性单独传 MULTI_THREAD。
    ExecutionMode execution_mode = ExecutionMode::SINGLE_THREAD;

    size_t thread_count = 1;
    size_t scan_threads = 1;
    size_t compute_threads = 1;
    size_t queue_capacity = 16;

    // 内存消融：SYSTEM(new_delete) vs ARENA；DIRECT vs POOLED。
    QueryMemoryMode memory_mode = QueryMemoryMode::ARENA;
    BufferPoolMode buffer_pool_mode = BufferPoolMode::POOLED;

    size_t block_pool_cache = 64;
    size_t buffer_pool_cache = 64;

    std::string workload = "mixed";

    bool prepare_data = false;
};

// ==========================================================
// 延迟统计
// ==========================================================

// P99 至少需要这么多样本才有意义；更少时 benchmark 必须声明不作为 P99。
inline constexpr uint64_t kMinSamplesForP99 = 1000;

struct LatencyStats {
    uint64_t query_count = 0;

    double elapsed_seconds = 0.0;
    double qps = 0.0;

    double mean_us = 0.0;

    uint64_t min_us = 0;
    uint64_t p50_us = 0;
    uint64_t p95_us = 0;
    uint64_t p99_us = 0;
    uint64_t max_us = 0;

    bool p99_reliable() const {
        return query_count >= kMinSamplesForP99;
    }
};

// nearest-rank 百分位；samples 必须已排序。
inline uint64_t PercentileNearestRank(const std::vector<uint64_t>& sorted_ns, double percentile) {
    if (sorted_ns.empty()) {
        return 0;
    }
    const size_t rank = static_cast<size_t>(std::ceil(percentile * static_cast<double>(sorted_ns.size())));
    const size_t index = rank == 0 ? 0 : rank - 1;
    return sorted_ns[std::min(index, sorted_ns.size() - 1)];
}

inline LatencyStats ComputeLatencyStats(std::vector<uint64_t> samples_ns, double elapsed_seconds) {
    LatencyStats stats;
    stats.query_count = samples_ns.size();
    stats.elapsed_seconds = elapsed_seconds;

    if (stats.query_count == 0 || elapsed_seconds <= 0.0) {
        return stats;
    }

    // QPS 严格定义：measurement window 内完成的 query 数 / 真实 elapsed 秒数，
    // 绝不使用 1000 / 单次 latency 推导。
    stats.qps = static_cast<double>(stats.query_count) / elapsed_seconds;

    std::sort(samples_ns.begin(), samples_ns.end());

    long double total_ns = 0.0L;
    for (uint64_t ns : samples_ns) {
        total_ns += static_cast<long double>(ns);
    }
    stats.mean_us = static_cast<double>(total_ns / static_cast<long double>(stats.query_count)) / 1000.0;

    stats.min_us = samples_ns.front() / 1000;
    stats.p50_us = PercentileNearestRank(samples_ns, 0.50) / 1000;
    stats.p95_us = PercentileNearestRank(samples_ns, 0.95) / 1000;
    stats.p99_us = PercentileNearestRank(samples_ns, 0.99) / 1000;
    stats.max_us = samples_ns.back() / 1000;
    return stats;
}

// ==========================================================
// 微基准 harness
// ==========================================================
//
// 约定：fn(inner_iterations) 在一次调用内执行 inner_iterations 次被测操作；
// harness 每个 sample 只计时一次 fn(inner)，因此单次 std::function/循环开销
// 可以被 inner 摊薄，但 kernel 本体不会被额外代码污染。
//
// 主要报告 median 与 MAD（median absolute deviation），不把 best 当主指标；
// 多 kernel 对比使用 RunInterleavedMicroBenchmark：每轮以固定种子随机顺序
// 执行各 kernel，消除「A 总在 B 前 / B 总在 A 前」带来的频率、温度、cache 偏差。

struct BenchmarkStats {
    uint32_t samples = 0;

    double min_ns = 0.0;
    double median_ns = 0.0;
    double mean_ns = 0.0;
    double mad_ns = 0.0;
    double p95_ns = 0.0;
    double max_ns = 0.0;
};

inline double MedianOf(std::vector<double> values) {
    if (values.empty()) {
        return 0.0;
    }
    std::sort(values.begin(), values.end());
    const size_t n = values.size();
    return (n % 2 == 1) ? values[n / 2] : 0.5 * (values[n / 2 - 1] + values[n / 2]);
}

inline BenchmarkStats ComputeBenchmarkStats(std::vector<double> samples) {
    BenchmarkStats stats;
    stats.samples = static_cast<uint32_t>(samples.size());
    if (samples.empty()) {
        return stats;
    }

    stats.median_ns = MedianOf(samples);
    stats.mean_ns = std::accumulate(samples.begin(), samples.end(), 0.0) / static_cast<double>(samples.size());

    std::vector<double> deviations;
    deviations.reserve(samples.size());
    for (double value : samples) {
        deviations.push_back(std::fabs(value - stats.median_ns));
    }
    stats.mad_ns = MedianOf(std::move(deviations));

    std::sort(samples.begin(), samples.end());
    stats.min_ns = samples.front();
    stats.max_ns = samples.back();
    stats.p95_ns = samples[std::min(samples.size() - 1,
                                     static_cast<size_t>(std::ceil(0.95 * static_cast<double>(samples.size()))) - 1)];
    return stats;
}

// 阻止编译器把被测结果优化掉（不会引入实际内存操作）。
template <typename T> inline void DoNotOptimize(const T& value) {
#if defined(__GNUC__) || defined(__clang__)
    asm volatile("" : : "g"(value) : "memory");
#else
    (void)value;
#endif
}

inline void ClobberMemory() {
#if defined(__GNUC__) || defined(__clang__)
    asm volatile("" : : : "memory");
#endif
}

// 单 kernel：warmup_rounds 轮丢弃，measured_rounds 轮记录。
template <typename Fn>
inline BenchmarkStats RunMicroBenchmark(Fn&& fn, uint32_t warmup_rounds, uint32_t measured_rounds,
                                        uint64_t inner_iterations, uint64_t seed = 20260919ULL) {
    (void)seed;
    std::vector<double> samples;
    samples.reserve(measured_rounds);

    for (uint32_t round = 0; round < warmup_rounds; ++round) {
        fn(inner_iterations);
    }

    for (uint32_t round = 0; round < measured_rounds; ++round) {
        const uint64_t begin = NowNs();
        fn(inner_iterations);
        const uint64_t end = NowNs();
        samples.push_back(static_cast<double>(end - begin) / static_cast<double>(inner_iterations));
    }

    return ComputeBenchmarkStats(std::move(samples));
}

struct MicroKernel {
    std::string name;
    std::function<void(uint64_t)> fn;
};

// 多 kernel 交错：每轮随机顺序执行所有 kernel，返回与 kernels 一一对应的 stats。
inline std::vector<BenchmarkStats> RunInterleavedMicroBenchmark(const std::vector<MicroKernel>& kernels,
                                                                uint32_t warmup_rounds, uint32_t measured_rounds,
                                                                uint64_t inner_iterations,
                                                                uint64_t seed = 20260919ULL) {
    std::vector<std::vector<double>> samples(kernels.size());

    std::vector<size_t> order(kernels.size());
    std::iota(order.begin(), order.end(), size_t{0});

    std::mt19937_64 rng(seed);
    const auto run_round = [&](bool measured) {
        std::shuffle(order.begin(), order.end(), rng);
        for (size_t index : order) {
            const uint64_t begin = NowNs();
            kernels[index].fn(inner_iterations);
            const uint64_t end = NowNs();
            if (measured) {
                samples[index].push_back(static_cast<double>(end - begin) / static_cast<double>(inner_iterations));
            }
        }
    };

    for (uint32_t round = 0; round < warmup_rounds; ++round) {
        run_round(false);
    }
    for (uint32_t round = 0; round < measured_rounds; ++round) {
        run_round(true);
    }

    std::vector<BenchmarkStats> result;
    result.reserve(kernels.size());
    for (auto& sample : samples) {
        result.push_back(ComputeBenchmarkStats(std::move(sample)));
    }
    return result;
}

// ==========================================================
// 内存池统计（只在测量窗口内取增量）
// ==========================================================

struct MemoryStats {
    BlockPoolStats block_before;
    BlockPoolStats block_after;

    BufferPoolStats buffer_before;
    BufferPoolStats buffer_after;

    uint64_t block_system_alloc() const {
        return block_after.system_allocations - block_before.system_allocations;
    }
    uint64_t block_system_free() const {
        return block_after.system_deallocations - block_before.system_deallocations;
    }
    uint64_t block_hits() const {
        return block_after.pool_hits - block_before.pool_hits;
    }
    uint64_t block_returns() const {
        return block_after.pool_returns - block_before.pool_returns;
    }

    uint64_t buffer_system_alloc() const {
        return buffer_after.system_allocations - buffer_before.system_allocations;
    }
    uint64_t buffer_system_free() const {
        return buffer_after.system_deallocations - buffer_before.system_deallocations;
    }
    uint64_t buffer_hits() const {
        return buffer_after.pool_hits - buffer_before.pool_hits;
    }
    uint64_t buffer_returns() const {
        return buffer_after.pool_returns - buffer_before.pool_returns;
    }

    double pool_hit_rate() const {
        const uint64_t hits = block_hits() + buffer_hits();
        const uint64_t system = block_system_alloc() + buffer_system_alloc();
        const uint64_t total = hits + system;
        return total == 0 ? 0.0 : static_cast<double>(hits) / static_cast<double>(total);
    }
};

// ==========================================================
// 运行环境
// ==========================================================

struct EnvironmentInfo {
    std::string git_commit = SIMPLE_OLAP_GIT_COMMIT;
    std::string build_type = SIMPLE_OLAP_BUILD_TYPE;
    std::string compiler;
    std::string cpu_model = "unknown";
    size_t logical_cores = 1;
    size_t physical_cores = 1;
    bool avx2_available = false;
    uint32_t vector_batch_rows = kVectorBatchSize;
};

inline EnvironmentInfo QueryEnvironment() {
    EnvironmentInfo info;

#if defined(__clang__)
    info.compiler = std::string("clang ") + __clang_version__;
#elif defined(__GNUC__)
    info.compiler = std::string("gcc ") + __VERSION__;
#else
    info.compiler = "unknown compiler";
#endif

    info.logical_cores = std::max<size_t>(1, std::thread::hardware_concurrency());

#if defined(__x86_64__) || defined(__i386__)
    __builtin_cpu_init();
    info.avx2_available = __builtin_cpu_supports("avx2");
#endif

    // 物理核数量 / CPU model：只依赖 /proc/cpuinfo，失败时回退逻辑核数。
    std::ifstream cpuinfo("/proc/cpuinfo");
    if (cpuinfo) {
        std::set<std::pair<std::string, std::string>> physical_cores;
        std::string line;
        std::string physical_id = "0";
        std::string core_id;
        while (std::getline(cpuinfo, line)) {
            const size_t colon = line.find(':');
            if (colon == std::string::npos) {
                continue;
            }
            std::string key = line.substr(0, colon);
            std::string value = line.substr(colon + 1);
            const size_t begin = value.find_first_not_of(" \t");
            const size_t end = value.find_last_not_of(" \t");
            value = (begin == std::string::npos) ? std::string() : value.substr(begin, end - begin + 1);

            if (key.find("model name") != std::string::npos && info.cpu_model == "unknown") {
                info.cpu_model = value;
            } else if (key.find("physical id") != std::string::npos) {
                physical_id = value;
            } else if (key.find("core id") != std::string::npos) {
                core_id = value;
            }

            if (key.find("processor") != std::string::npos) {
                if (!core_id.empty()) {
                    physical_cores.insert({physical_id, core_id});
                    core_id.clear();
                }
            }
        }
        if (!core_id.empty()) {
            physical_cores.insert({physical_id, core_id});
        }
        if (!physical_cores.empty()) {
            info.physical_cores = physical_cores.size();
        }
    }
    if (info.physical_cores == 0) {
        info.physical_cores = info.logical_cores;
    }

    return info;
}

inline const char* ExecutionModeName(ExecutionMode mode) {
    switch (mode) {
    case ExecutionMode::SINGLE_THREAD:
        return "single";
    case ExecutionMode::MULTI_THREAD:
        return "multi";
    case ExecutionMode::AUTO:
        return "auto";
    }
    return "unknown";
}

inline const char* MemoryModeName(QueryMemoryMode mode) {
    return mode == QueryMemoryMode::ARENA ? "arena" : "system";
}

inline const char* BufferPoolModeName(BufferPoolMode mode) {
    return mode == BufferPoolMode::POOLED ? "pooled" : "direct";
}

inline void PrintEnvironment(const EnvironmentInfo& info) {
    std::printf("environment: commit=%s build=%s compiler=%s\n", info.git_commit.c_str(), info.build_type.c_str(),
                info.compiler.c_str());
    std::printf("environment: cpu=\"%s\" logical_cores=%zu physical_cores=%zu avx2=%s vector_batch_rows=%u\n",
                info.cpu_model.c_str(), info.logical_cores, info.physical_cores,
                info.avx2_available ? "yes" : "no", info.vector_batch_rows);
}

} // namespace simple_olap::bench
