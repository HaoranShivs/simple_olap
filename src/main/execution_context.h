#pragma once

#include <cstddef>
#include <filesystem>
#include <thread>

#include "../execution/execution_context.h" // ExecutionMode
#include "../parallel/parallel_config.h"    // ParallelConfig

namespace simple_olap {

// 数据库级配置。
struct DatabaseConfig {
    // 线程池大小；默认取硬件并发度，取不到时为 1。
    size_t thread_count = [] {
        const auto n = std::thread::hardware_concurrency();
        return n == 0 ? 1 : static_cast<size_t>(n);
    }();

    // Arena block 大小（传给 BlockPool）。
    size_t arena_block_size = 1 << 20; // 1 MB

    // BlockPool freelist 缓存上限。
    size_t block_pool_max_cached_blocks = 64;

    // BufferPool 每个 size class 的缓存上限。
    size_t buffer_pool_max_cached_per_class = 64;

    // 默认执行模式：由 Connection 注入到每条语句的 ExecutionContext。
    // 可在运行期切换以对比同一 SQL 的单线程 / 多线程执行。
    ExecutionMode execution_mode = ExecutionMode::AUTO;

    // 并行执行参数：由 Connection 注入到每条语句的 ExecutionContext，
    // 最终决定 ParallelExecutor 的 scan 线程数 / compute 线程数 / 队列容量。
    ParallelConfig parallel_config;
};

} // namespace simple_olap