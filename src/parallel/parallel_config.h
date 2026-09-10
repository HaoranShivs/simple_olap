#pragma once

#include <cstddef>

namespace simple_olap {

// 并行执行配置（第一版为静态配置，不做动态调整）。
struct ParallelConfig {
    size_t scan_threads = 2;          // storage 扫描线程数（Segment -> BatchQueue）
    size_t compute_threads = 4;       // compute 线程数（BatchQueue -> 局部聚合）
    size_t batch_queue_capacity = 64; // BatchQueue 容量，用于背压
};

} // namespace simple_olap
