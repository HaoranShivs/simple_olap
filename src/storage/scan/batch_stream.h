#pragma once

#include <memory>

#include "../../execution/vector/vector.h"

namespace simple_olap {

// 批次流抽象：执行层（SeqScanOperator）从这里拉取扫描批次。
//
// 两种实现来源：
//   - 串行路径：不经过 BatchStream，SeqScanOperator 直接调 TableStorage::Scan
//   - 并行路径：ParallelScanSession（storage scan 线程池 -> BoundedBlockingQueue）
//
// 语义：
//   Next(batch) == true  -> batch 携带一个非空批次
//   Next(batch) == false -> EOF（正常结束或被取消）
//   Cancel()             -> 提前终止：唤醒所有阻塞的 scan worker 并丢弃队列数据
class BatchStream {
  public:
    virtual ~BatchStream() = default;

    virtual bool Next(VectorBatch& batch) = 0;

    virtual void Cancel() = 0;
};

} // namespace simple_olap
