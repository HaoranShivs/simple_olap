#pragma once

#include <cstdint>
#include <memory>
#include <string_view>

#include "../execution/execution_engine.h" // BatchConsumer
#include "database.h"
#include "query_result.h"

namespace simple_olap {

// 流式执行摘要：不物化结果集时用于校验执行完整性。
//
//   QUERY   -> row_count
//   COMMAND -> affected_rows
struct ExecutionSummary {
    uint64_t row_count = 0;
    uint64_t affected_rows = 0;
};

// PreparedQuery：一个只读 SQL 的预编译结果。
//
// 只保存 SQL 前端（Lexer/Parser/Binder）与 planning（Logical/Optimizer/Physical）
// 的产物；执行期内存、ExecutionContext、算子树都在每次 Execute 时重新创建。
// 这样 Connection::Execute(prepared, consumer) 只包含：
//     ExecutionEngine -> Operator Tree -> Execution
// 用于把「SQL frontend + planning」与「执行」分开计时。
class PreparedQuery {
  public:
    PreparedQuery() noexcept;
    ~PreparedQuery();

    PreparedQuery(PreparedQuery&& other) noexcept;
    PreparedQuery& operator=(PreparedQuery&& other) noexcept;

    PreparedQuery(const PreparedQuery&) = delete;
    PreparedQuery& operator=(const PreparedQuery&) = delete;

    bool valid() const noexcept {
        return impl_ != nullptr;
    }

  private:
    friend class Connection;

    struct Impl;

    explicit PreparedQuery(std::unique_ptr<Impl> impl) noexcept;

    std::unique_ptr<Impl> impl_;
};

// Connection 是一次查询会话的入口：
//   SQL 文本 -> Lexer -> Parser -> Binder -> Planner -> Optimizer
//     -> PhysicalPlanner -> ExecutorBuilder -> 执行
// DDL（CREATE TABLE / DROP TABLE）由 Database 直接协调 Catalog 与 StorageManager。
class Connection {
  public:
    explicit Connection(Database& database) : database_(database) {}

    // 完整物化路径：执行并把每个 batch 深拷贝进 QueryResult。
    QueryResult Query(std::string_view sql);

    // 流式执行路径：结果只通过 consumer 回调，不构造 QueryResult，
    // 用于度量「纯执行引擎」延迟（engine latency / engine QPS）。
    ExecutionSummary Execute(std::string_view sql, const BatchConsumer& consumer);

    // 预编译：SQL 前端 + planning 只做一次。
    PreparedQuery Prepare(std::string_view sql);

    // 执行已预编译查询：只包含执行阶段。
    ExecutionSummary Execute(const PreparedQuery& query, const BatchConsumer& consumer);

    Database& GetDatabase() {
        return database_;
    }

  private:
    Database& database_; // 所属数据库（Catalog / StorageManager / 内存池的持有者）
};

} // namespace simple_olap
