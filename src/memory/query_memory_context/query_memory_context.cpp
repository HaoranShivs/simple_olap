#include "query_memory_context.h"

#include <stdexcept>

namespace simple_olap {

QueryMemoryContext::QueryMemoryContext(BlockPool& block_pool, size_t worker_count, QueryMemoryMode mode)
    : mode_(mode), worker_count_(worker_count) {
    if (mode_ != QueryMemoryMode::ARENA) {
        // SYSTEM：不创建 Arena，资源接口直接返回 new_delete_resource()。
        return;
    }

    coordinator_arena_ = std::make_unique<Arena>(block_pool);

    worker_arenas_.reserve(worker_count_);
    for (size_t i = 0; i < worker_count_; ++i) {
        worker_arenas_.push_back(std::make_unique<Arena>(block_pool));
    }
}

std::pmr::memory_resource* QueryMemoryContext::CoordinatorResource() noexcept {
    if (mode_ != QueryMemoryMode::ARENA || coordinator_arena_ == nullptr) {
        return std::pmr::new_delete_resource();
    }
    return coordinator_arena_.get();
}

std::pmr::memory_resource* QueryMemoryContext::WorkerResource(size_t worker_id) {
    if (worker_id >= worker_count_) {
        throw std::out_of_range("QueryMemoryContext::WorkerResource: worker_id out of range");
    }
    if (mode_ != QueryMemoryMode::ARENA) {
        return std::pmr::new_delete_resource();
    }
    return worker_arenas_[worker_id].get();
}

} // namespace simple_olap
