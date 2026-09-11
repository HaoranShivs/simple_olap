#pragma once

#include <string_view>

#include "database.h"
#include "query_result.h"

namespace simple_olap {

// Connection 是一次查询会话的入口：
//   SQL 文本 -> Lexer -> Parser -> Binder -> Planner -> Optimizer
//     -> PhysicalPlanner -> ExecutorBuilder -> 执行
// DDL（CREATE TABLE / DROP TABLE）由 Database 直接协调 Catalog 与 StorageManager。
class Connection {
  public:
    explicit Connection(Database& database) : database_(database) {}

    QueryResult Query(std::string_view sql);

    Database& GetDatabase() {
        return database_;
    }

  private:
    Database& database_; // 所属数据库（Catalog / StorageManager / 内存池的持有者）
};

} // namespace simple_olap
