#pragma once

#include "../catalog/catalog.h"
#include "../memory/arena/arena.h"
#include "../parallel/thread_pool/thread_pool.h"
#include "../storage/storage_manager.h"

namespace simple_olap {

// 执行层上下文：Catalog 与 StorageManager 平级注入。
// bind 阶段用 Catalog（表/列名 -> id），scan 阶段用 StorageManager（id -> 物理表）。
struct ExecutionContext {
    Catalog* catalog = nullptr;

    StorageManager* storage_manager = nullptr;

    Arena* arena = nullptr;

    ThreadPool* thread_pool = nullptr;

    // 引用式构造：成员仍以指针存储，执行层统一用 -> 访问
    ExecutionContext(Catalog& catalog_ref, StorageManager& storage_ref, ThreadPool& pool_ref, Arena& arena_ref)
        : catalog(&catalog_ref), storage_manager(&storage_ref), arena(&arena_ref), thread_pool(&pool_ref) {}

    // 默认构造：全部置空，逐字段注入
    ExecutionContext() = default;
};

} // namespace simple_olap
