#pragma once

#include <cstddef>

namespace simple_olap {

// 并行执行配置（第一版为静态配置，不做动态调整）。
//
// 旧的两阶段架构（scan threads + compute threads）已被 morsel-driven
// pipeline worker 取代：scan / filter / projection / partial aggregate
// 全部由同一个 worker 连续执行，因此只有一个并行度概念。
struct ParallelConfig {
    // 一条查询最多使用的 pipeline worker 数。
    // 实际 worker 数还会被 thread pool 大小与 segment 数截断。
    size_t worker_threads = 4;

    // 仅非聚合并行查询的「最终结果交接队列」容量（背压）；
    // aggregate pipeline 在 worker 内完成局部聚合，不使用该队列。
    size_t result_queue_capacity = 64;
};

} // namespace simple_olap
