#pragma once

#include <cstddef>
#include <memory>
#include <vector>

#include "../arena/arena.h"

namespace simple_olap {

// QueryMemoryContext：一条 SQL 一个实例。
//   - coordinator_arena_：coordinator 线程使用（串行执行、merge、输出）
//   - worker_arenas_    ：compute worker N 使用 WorkerArena(N)
//
// worker_arenas_ 必须 unique_ptr，保证 Arena 地址稳定（vector 扩容不移动）。
class QueryMemoryContext {
  public:
    QueryMemoryContext(BlockPool& block_pool, size_t worker_count);

    QueryMemoryContext(const QueryMemoryContext&) = delete;
    QueryMemoryContext& operator=(const QueryMemoryContext&) = delete;

    Arena& CoordinatorArena() noexcept {
        return coordinator_arena_;
    }

    Arena& WorkerArena(size_t worker_id);

    size_t worker_count() const noexcept {
        return worker_arenas_.size();
    }

  private:
    Arena coordinator_arena_;

    // 必须 unique_ptr，保证 Arena 地址稳定。
    std::vector<std::unique_ptr<Arena>> worker_arenas_;
};

} // namespace simple_olap
