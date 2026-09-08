#pragma once

#include <filesystem>
#include <memory>
#include <unordered_map>

#include "../type.h"
#include "table/table_storage.h"

namespace simple_olap {

class TableSchema;

// database 级物理存储管理器：管理整个数据库的物理存储。
//
//   database physical storage
//             ├── table 1 (TableStorage)
//             ├── table 2 (TableStorage)
//             └── table 3 (TableStorage)
//
// 与 Catalog 平级：Catalog 管"这张表是什么"（name / schema），
// StorageManager 管"数据在哪里、怎么读写"（tables/{id}/ 下的 segment）。
// 两者只通过 TableId 关联，互不感知；由 Database 协调二者。
// 将来 BufferManager / BlockManager / WAL / Checkpoint 也挂在这里。
class StorageManager {
  public:
    explicit StorageManager(std::filesystem::path db_path);

    StorageManager(const StorageManager&) = delete;
    StorageManager& operator=(const StorageManager&) = delete;

    ~StorageManager();

    // 获取（必要时打开）指定表的物理存储；表不存在返回 nullptr。
    // schema 由调用方从 Catalog 取出后传入（StorageManager 不感知 Catalog）。
    std::shared_ptr<TableStorage> GetTable(TableId table_id, const TableSchema& schema);

    // 创建新表的物理存储：tables/{table_id}/ 目录 + table.meta
    std::shared_ptr<TableStorage> CreateTable(TableId table_id, const TableSchema& schema);

    // 删除表的物理存储：落盘后删除 tables/{table_id}/ 目录
    bool DropTable(TableId table_id);

    // 刷盘所有已打开表的内存 segment
    void Flush();

    // ---------- 观察接口 ----------

    // 数据库根目录
    const std::filesystem::path& path() const noexcept {
        return root_path_;
    }

    // 当前已打开的表数量
    size_t open_table_count() const noexcept {
        return tables_.size();
    }

  private:
    std::filesystem::path TablesRoot() const;

    std::filesystem::path root_path_;

    // 已打开表的物理存储缓存：table_id -> TableStorage
    std::unordered_map<TableId, std::shared_ptr<TableStorage>> tables_;
};

} // namespace simple_olap
