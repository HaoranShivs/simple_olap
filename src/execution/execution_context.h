#pragma once

#include <cstdint>

#include "../catalog/catalog.h"
#include "../memory/arena/arena.h"
#include "../parallel/parallel_config.h"
#include "../parallel/thread_pool/thread_pool.h"
#include "../storage/storage_manager.h"

namespace simple_olap {

// 执行模式：显式决定一条 SQL 语句走单线程还是多线程执行。
//
//   AUTO          —— 按计划能力自动选择：可并行聚合走多线程，其余串行。
//                    （保持既有默认行为，不改变历史结果。）
//   SINGLE_THREAD —— 强制串行：始终走 Volcano pull 单线程路径。
//   MULTI_THREAD  —— 强制多线程：可并行的计划走并行路径；
//                    计划形状不支持并行或无可用线程池时回退串行。
//
// 单线程/多线程两条路径共享同一份算子与聚合中间态实现，只改变调度方式，
// 因此同一 SQL 在两种模式下的结果语义一致（多线程输出批次顺序不做保证）。
enum class ExecutionMode : uint8_t {
    AUTO = 0,
    SINGLE_THREAD = 1,
    MULTI_THREAD = 2,
};

// 执行层上下文：Catalog 与 StorageManager 平级注入。
// bind 阶段用 Catalog（表/列名 -> id），scan 阶段用 StorageManager（id -> 物理表）。
struct ExecutionContext {
    Catalog* catalog = nullptr;

    StorageManager* storage_manager = nullptr;

    Arena* arena = nullptr;

    ThreadPool* thread_pool = nullptr;

    // 本条语句的执行模式，由上层（Connection）按需设置。
    ExecutionMode execution_mode = ExecutionMode::AUTO;

    // 并行执行参数（scan 线程数 / compute 线程数 / 批队列容量）。
    // 由上层（Connection）从 DatabaseConfig 注入，用于对比不同并行配置。
    ParallelConfig parallel_config;

    // 引用式构造：成员仍以指针存储，执行层统一用 -> 访问
    ExecutionContext(Catalog& catalog_ref, StorageManager& storage_ref, ThreadPool& pool_ref, Arena& arena_ref)
        : catalog(&catalog_ref), storage_manager(&storage_ref), arena(&arena_ref), thread_pool(&pool_ref) {}

    // 默认构造：全部置空，逐字段注入
    ExecutionContext() = default;
};

} // namespace simple_olap
