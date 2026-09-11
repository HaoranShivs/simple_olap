#include "query_memory_context.h"

#include <stdexcept>

namespace simple_olap {

QueryMemoryContext::QueryMemoryContext(BlockPool& block_pool, size_t worker_count) : coordinator_arena_(block_pool) {
    worker_arenas_.reserve(worker_count);

    for (size_t i = 0; i < worker_count; ++i) {
        worker_arenas_.push_back(std::make_unique<Arena>(block_pool));
    }
}

Arena& QueryMemoryContext::WorkerArena(size_t worker_id) {
    if (worker_id >= worker_arenas_.size()) {
        throw std::out_of_range("QueryMemoryContext::WorkerArena: worker_id out of range");
    }
    return *worker_arenas_[worker_id];
}

} // namespace simple_olap
