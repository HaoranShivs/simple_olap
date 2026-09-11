#pragma once

#include <atomic>
#include <memory>
#include <thread>
#include <vector>

#include "../../parallel/queue/bounded_blocking_queue.h"
#include "../../parallel/thread_pool/thread_pool.h"
#include "../datastructs.h"
#include "batch_stream.h"

namespace simple_olap {

class TableStorage;

// 并行扫描会话：storage 侧的 BatchStream 实现。
//
// 结构：
//
//   scan worker 线程（storage scan pool）
//     S0  S1  S2...   <- atomic next_segment_ 领取互不重叠的 segment
//        |            <- TableStorage::ScanSegment（与串行路径同一份扫描逻辑）
//        v
//   BoundedBlockingQueue<VectorBatch>（背压）
//        |
//        v
//   BatchStream::Next()（compute 线程 / SeqScanOperator 拉取）
//
// 生命周期：
//   Start()  启动 scan worker
//   Next()   拉取批次；EOF 返回 false
//   Cancel() 提前终止：唤醒 worker、丢弃队列数据
//   析构     自动 Cancel + join，保证不泄漏线程
class ParallelScanSession final : public BatchStream {
  public:
    ParallelScanSession(TableStorage* table, std::vector<SegmentId> segment_ids, ScanOptions options,
                        size_t scan_threads, size_t queue_capacity, BufferPool* buffer_pool);

    ~ParallelScanSession() override;

    ParallelScanSession(const ParallelScanSession&) = delete;
    ParallelScanSession& operator=(const ParallelScanSession&) = delete;

    // 启动 scan worker 线程（幂等：重复调用为 no-op）
    void Start();

    // 拉取一个批次；EOF（所有 segment 扫完）返回 false。
    // scan worker 抛出的异常在此 rethrow。
    bool Next(VectorBatch& batch) override;

    // 提前终止：唤醒所有 worker 并丢弃队列数据
    void Cancel() override;

  private:
    // scan worker 主循环：atomic 领取 segment -> ScanSegment -> Push
    void ScanWorkerLoop();

    TableStorage* table_;

    // scan worker 产出的 VectorBatch 绑定此 BufferPool；
    // BufferHandle 随 batch move 进队列，最终由消费方归还。
    BufferPool* buffer_pool_ = nullptr;

    BoundedBlockingQueue<VectorBatch> queue_;

    std::vector<SegmentId> segment_ids_;
    std::atomic<size_t> next_segment_{0};

    std::atomic<size_t> active_workers_{0};
    std::atomic<bool> cancelled_{false};
    std::atomic<bool> started_{false};

    ScanOptions options_;

    // storage scan pool：只服务本 session 的 scan worker
    ThreadPool scan_pool_;
};

} // namespace simple_olap
