#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

#include "../sql/ast/statement.h" // CreateTableStatement
#include "../storage/datastructs.h"
#include "../type.h"
#include "table_catalog_entry.h"

namespace simple_olap {

// 表目录：逻辑元数据（表名 / table_id / schema）的唯一权威来源。
// 不持有任何运行中的物理存储对象，不感知 Segment / mmap / 磁盘布局。
// 物理存储由平级的 StorageManager 负责，两者只通过 TableId 关联；
// 由更上层的 Database（或 DDL Executor）协调二者。
class Catalog {
  public:
    // ---------- 持久化 ----------

    // 创建 catalog 根目录并写入空元数据；已存在则拒绝
    bool Create(const std::filesystem::path& path);

    // 从 catalog 根目录读取 catalog.meta
    bool LoadMeta(const std::filesystem::path& path);

    // 把内存元数据写回 catalog 根目录
    bool SaveMeta() const;

    // ---------- 表变更 ----------

    // 创建新表条目：column_id 按列顺序从 0 分配，table_id 自动分配。
    // 成功后立即持久化 catalog.meta；失败返回 false 且不留副作用。
    bool CreateTable(const CreateTableStatement& stmt);

    // 删除表条目（物理数据目录由 StorageManager::DropTable 负责）
    bool DropTable(std::string_view table_name);

    // ---------- 查询（binder / optimizer 高频路径，纯内存） ----------

    // 表是否存在：存在返回 table_id，否则返回空 optional
    std::optional<TableId> FindTable(std::string_view table_name) const;

    // 按表名取目录条目；不存在返回 nullptr
    const TableCatalogEntry* GetTable(std::string_view table_name) const;

    // 按 table_id 取目录条目；不存在返回 nullptr
    const TableCatalogEntry* GetTable(TableId table_id) const;

    // ---------- 调试/REPL 观察 ----------

    // 表名 -> table_id 映射（只读）
    const std::unordered_map<std::string, TableId>& name_index() const noexcept {
        return name_index_;
    }

  private:
    // 分配下一个可用的 table_id（现有最大值 + 1）。
    TableId NextTableId() const;

    // name -> table_id 索引
    std::unordered_map<std::string, TableId> name_index_;

    // table_id -> 目录条目（schema 的权威来源）
    std::unordered_map<TableId, TableCatalogEntry> tables_;

    std::filesystem::path root_path_;
};

} // namespace simple_olap
