#pragma once

#include <filesystem>
#include <memory>
#include <string_view>

#include "../catalog/catalog.h"
#include "../memory/block_pool/block_pool.h"
#include "../memory/buffer_pool/buffer_pool.h"
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
    // path 为数据库根目录；已存在则加载，否则新建。
    explicit Database(std::filesystem::path path, DatabaseConfig config = {});

    ~Database();

    Database(const Database&) = delete;
    Database& operator=(const Database&) = delete;

    // ---------- 访问器 ----------

    Catalog& GetCatalog() {
        return catalog_;
    }

    StorageManager& GetStorageManager() {
        return storage_manager_;
    }

    ThreadPool& GetThreadPool() {
        return thread_pool_;
    }

    BlockPool& GetBlockPool() {
        return block_pool_;
    }

    BufferPool& GetBufferPool() {
        return buffer_pool_;
    }

    const DatabaseConfig& GetConfig() const {
        return config_;
    }

    // 运行期切换执行模式：只影响后续查询，可反复在单线程/多线程间对比。
    void SetExecutionMode(ExecutionMode mode) {
        config_.execution_mode = mode;
    }

    ExecutionMode GetExecutionMode() const {
        return config_.execution_mode;
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
    // 加载已有元数据；不存在则创建空的 catalog。
    void Initialize();

  private:
    std::filesystem::path path_; // 数据库根目录
    DatabaseConfig config_;

    // 内存池：必须比 QueryMemoryContext / VectorBatch / ParallelScanSession 活得更久。
    // 注意成员初始化顺序：block_pool_ / buffer_pool_ 在 storage_manager_ 之前构造。
    BlockPool block_pool_;
    BufferPool buffer_pool_;

    // 以下三者平级：逻辑元数据 / 物理存储 / 执行线程池。
    Catalog catalog_;
    StorageManager storage_manager_;
    ThreadPool thread_pool_;
};

} // namespace simple_olap
