#pragma once

namespace simple_olap {

class Catalog;

struct ExecutionContext {
    Catalog* catalog = nullptr;

    // 后续再扩展
    // Arena *arena;
    // ThreadPool *thread_pool;
};

} // namespace simple_olap
