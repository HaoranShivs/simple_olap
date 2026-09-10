#include "parallel_scan_session.h"

#include <exception>

#include "../table/table_storage.h"

namespace simple_olap {

ParallelScanSession::ParallelScanSession(TableStorage* table, std::vector<SegmentId> segment_ids, ScanOptions options,
                                         size_t scan_threads, size_t queue_capacity)
    : table_(table), queue_(queue_capacity), segment_ids_(std::move(segment_ids)), options_(std::move(options)),
      scan_pool_(scan_threads == 0 ? 1 : scan_threads) {}

ParallelScanSession::~ParallelScanSession() {
    // 兜底：唤醒所有阻塞在队列上的线程，再 join scan worker
    Cancel();
}

void ParallelScanSession::Start() {
    bool expected = false;
    if (!started_.compare_exchange_strong(expected, true)) {
        return; // 已启动
    }

    const size_t worker_count = std::min(scan_pool_.thread_count(), segment_ids_.size());
    if (worker_count == 0) {
        // 没有 segment：直接 EOF
        queue_.Close();
        return;
    }

    active_workers_.store(worker_count, std::memory_order_relaxed);

    for (size_t i = 0; i < worker_count; ++i) {
        scan_pool_.Submit([this] { ScanWorkerLoop(); });
    }
}

bool ParallelScanSession::Next(VectorBatch& batch) {
    if (!started_.load(std::memory_order_relaxed)) {
        Start();
    }

    if (!queue_.Pop(batch)) {
        // EOF 或被取消：检查 scan worker 是否留下了异常
        if (std::exception_ptr error = queue_.Error()) {
            std::rethrow_exception(error);
        }
        return false;
    }
    return true;
}

void ParallelScanSession::Cancel() {
    cancelled_.store(true, std::memory_order_relaxed);
    queue_.Cancel();
}

void ParallelScanSession::ScanWorkerLoop() {
    try {
        while (!cancelled_.load(std::memory_order_relaxed)) {
            // atomic 领取下一个 segment：各 worker 互不重叠
            const size_t index = next_segment_.fetch_add(1, std::memory_order_relaxed);
            if (index >= segment_ids_.size()) {
                break;
            }

            const SegmentId id = segment_ids_[index];

            SegmentScanCursor cursor;
            VectorBatch batch;

            // 与串行路径共用同一份单 segment 扫描逻辑
            while (table_->ScanSegment(id, options_, cursor, batch)) {
                if (cancelled_.load(std::memory_order_relaxed)) {
                    return;
                }

                if (batch.size > 0 || !batch.columns.empty()) {
                    // 队列满时阻塞（背压）；Cancel/Close 后 Push 返回 false
                    if (!queue_.Push(std::move(batch))) {
                        return;
                    }
                }
                batch = VectorBatch{};
            }
        }
    } catch (...) {
        // 扫描异常：通知消费侧，并唤醒所有等待线程
        queue_.Abort(std::current_exception());
        active_workers_.fetch_sub(1, std::memory_order_relaxed);
        return;
    }

    // 最后一个退出的 worker 关闭队列（正常 EOF）
    if (active_workers_.fetch_sub(1, std::memory_order_acq_rel) == 1) {
        queue_.Close();
    }
}

} // namespace simple_olap
