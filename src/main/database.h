#pragma once

#include <filesystem>
#include <memory>
#include <string_view>

#include "../catalog/catalog.h"
#include "../parallel/thread_pool/thread_pool.h"
#include "../sql/ast/statement.h"
#include "../storage/storage_manager.h"
#include "execution_context.h" // DatabaseConfig

namespace simple_olap {

// Database 是 Catalog 与 StorageManager 的唯一协调点：
//
//   Database
//   ├── Catalog          管"这张表是什么"（name / schema）
//   ├── StorageManager   管"数据在哪里、怎么读写"（tables/{id}/ segment）
//   └── ThreadPool
//
// 两者平级、互不感知，只通过 TableId 关联。
// CREATE TABLE / DROP TABLE 等 DDL 由 Database 协调二者完成。
class Database {
  public:
    explicit Database(std::filesystem::path path, DatabaseConfig config = {});

    ~Database();

    Database(const Database&) = delete;
    Database& operator=(const Database&) = delete;

    Catalog& GetCatalog() {
        return catalog_;
    }

    StorageManager& GetStorageManager() {
        return storage_manager_;
    }

    ThreadPool& GetThreadPool() {
        return thread_pool_;
    }

    const DatabaseConfig& GetConfig() const {
        return config_;
    }

    const std::filesystem::path& GetPath() const {
        return path_;
    }

    // ---------- DDL 协调（Catalog 与 StorageManager 的唯一交汇处） ----------

    // 创建表：先在 Catalog 登记元数据，再创建物理存储
    bool CreateTable(const CreateTableStatement& stmt);

    // 删除表：先删 Catalog 条目，再由 StorageManager 标记删除物理目录
    bool DropTable(std::string_view table_name);

  private:
    void Initialize();

  private:
    std::filesystem::path path_;
    DatabaseConfig config_;

    Catalog catalog_;
    StorageManager storage_manager_;
    ThreadPool thread_pool_;
};

} // namespace simple_olap
