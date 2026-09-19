#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <memory_resource>
#include <vector>

#include "../arena/arena.h"

namespace simple_olap {

// 查询内存分配模式（用于 benchmark 消融，默认 ARENA，不改变生产行为）：
//
//   ARENA  ：coordinator / worker 各一个 Arena，基于 BlockPool 成块分配，
//            查询结束时整块归还（M1~M3）。
//   SYSTEM ：直接使用 std::pmr::new_delete_resource()，
//            等价于每个 PMR 分配都走 ::operator new/delete（M0）。
//
// 两种模式对 HashAggregateState 等 PMR 容器完全透明：它们只持有
// std::pmr::memory_resource*。
enum class QueryMemoryMode : uint8_t {
    ARENA = 0,
    SYSTEM = 1,
};

// QueryMemoryContext：一条 SQL 一个实例。
//   - coordinator resource：coordinator 线程使用（串行执行、merge、输出）
//   - worker resource     ：compute worker N 使用 WorkerResource(N)
//
// 返回 std::pmr::memory_resource* 而不是具体 Arena，让执行层不感知内存模式。
//
// worker_arenas_ 必须 unique_ptr，保证 Arena 地址稳定（vector 扩容不移动）。
class QueryMemoryContext {
  public:
    QueryMemoryContext(BlockPool& block_pool, size_t worker_count, QueryMemoryMode mode = QueryMemoryMode::ARENA);

    QueryMemoryContext(const QueryMemoryContext&) = delete;
    QueryMemoryContext& operator=(const QueryMemoryContext&) = delete;

    // coordinator 使用；SYSTEM 模式返回 new_delete_resource()。
    std::pmr::memory_resource* CoordinatorResource() noexcept;

    // worker_id 使用；SYSTEM 模式返回 new_delete_resource()。
    std::pmr::memory_resource* WorkerResource(size_t worker_id);

    size_t worker_count() const noexcept {
        return worker_count_;
    }

    QueryMemoryMode mode() const noexcept {
        return mode_;
    }

  private:
    QueryMemoryMode mode_ = QueryMemoryMode::ARENA;

    size_t worker_count_ = 0;

    // ARENA 模式下创建；SYSTEM 模式下为空。
    std::unique_ptr<Arena> coordinator_arena_;

    // 必须 unique_ptr，保证 Arena 地址稳定。
    std::vector<std::unique_ptr<Arena>> worker_arenas_;
};

} // namespace simple_olap
