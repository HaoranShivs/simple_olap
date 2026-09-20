#pragma once

#include <atomic>
#include <cstddef>
#include <memory>
#include <optional>
#include <vector>

#include "../datastructs.h"

namespace simple_olap {

// 前置声明：segment 只读视图（定义见 ../segment/segment.h）。
class SegmentReader;

// Query-local 并行扫描全局状态：morsel-driven 调度的唯一共享点。
//
// 生命周期：由 ParallelExecutor 在一条查询内创建，所有 pipeline worker
// 只读共享（next_segment 除外）。查询结束即销毁。
//
// 与旧的 ParallelScanSession 的区别：
//   - 不创建线程、不创建队列；
//   - Storage 只提供数据源（ScanParallel），调度由执行层 worker 自己完成；
//   - 每个 segment（最多 65536 行）只做一次 atomic fetch_add，
//     而不是每 1024 行做一次 queue Push/Pop + mutex/CV。
//
// GlobalState 绝不能挂到 TableStorage 上变为永久成员：
// 同一张表会被多个查询同时扫描，segment 分配器必须 per-query。
struct ParallelScanGlobalState {
    // 本次查询要扫描的 segment 列表（创建时从 table.meta 快照）。
    std::vector<SegmentId> segment_ids;

    // 下一个待领取的 segment 下标；worker 通过 ClaimSegment() 原子推进。
    std::atomic<size_t> next_segment{0};

    // 谓词计划：Build 一次，所有 worker 只读共享。
    // 为空表示没有 pushed predicate，ScanSegment 不会做行过滤。
    std::shared_ptr<const PreparedScanPredicates> prepared_predicates;

    // 取消标志：任一 worker 出错时置位，其余 worker 在领取 segment
    // 或下一次 ScanParallel() 时观察到并退出。
    std::atomic<bool> cancelled{false};

    // 可用并行度上界 = segment 数（1 segment -> 最多 1 个有效 worker）。
    size_t MaxThreads() const noexcept {
        return segment_ids.size();
    }

    // 领取下一个 segment；分配完毕或已取消时返回 nullopt。
    std::optional<SegmentId> ClaimSegment() noexcept {
        if (cancelled.load(std::memory_order_relaxed)) {
            return std::nullopt;
        }

        const size_t index = next_segment.fetch_add(1, std::memory_order_relaxed);
        if (index >= segment_ids.size()) {
            return std::nullopt;
        }
        return segment_ids[index];
    }

    void Cancel() noexcept {
        cancelled.store(true, std::memory_order_relaxed);
    }
};

// Worker-local 扫描状态：每个 pipeline worker 独有，不与任何线程共享。
//
// 关键点：reader 在 worker 领取 segment 时解析一次并缓存整个 segment，
// reader_cache_mutex_ 从 per-batch 锁下降到 per-segment 锁。
struct ParallelScanLocalState {
    SegmentId segment_id = kInvalidSegmentId;

    // 由 TableStorage::GetSegmentReader() 返回的缓存读者，生命周期覆盖查询。
    SegmentReader* reader = nullptr;

    // segment 内推进状态（offset / metadata 判断结果 / row filter mask）。
    SegmentScanCursor cursor;

    bool has_segment = false;

    // 当前 segment 读完（或被 pruning 跳过）后调用，下一次 ScanParallel()
    // 会领取新 segment。
    void ResetSegment() noexcept {
        segment_id = kInvalidSegmentId;
        reader = nullptr;
        cursor = SegmentScanCursor{};
        has_segment = false;
    }
};

} // namespace simple_olap
