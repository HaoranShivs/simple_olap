#include "database.h"

#include <system_error>

namespace simple_olap {

Database::Database(std::filesystem::path path, DatabaseConfig config)
    : path_(std::move(path)), config_(std::move(config)), catalog_(), storage_manager_(path_),
      thread_pool_(config_.thread_count) {

    Initialize();
}

Database::~Database() {
    // StorageManager 析构时会兜底落盘所有已打开表的内存 segment
}

void Database::Initialize() {
    if (!catalog_.LoadMeta(path_)) {
        if (!catalog_.Create(path_)) {
            throw std::runtime_error("Failed to create database: " + path_.string());
        }
    }
}

// ---------- DDL 协调（Catalog 与 StorageManager 的唯一交汇处） ----------

bool Database::CreateTable(const CreateTableStatement& stmt) {
    // 1. Catalog：登记逻辑元数据（name / schema / table_id）
    if (!catalog_.CreateTable(stmt)) {
        return false;
    }

    // 2. StorageManager：创建物理存储（tables/{table_id}/ + table.meta）
    const TableId table_id = catalog_.FindTable(stmt.table_name).value();
    const TableCatalogEntry* entry = catalog_.GetTable(table_id);
    const auto storage = storage_manager_.CreateTable(table_id, entry->schema);
    if (storage == nullptr) {
        // 物理创建失败：回滚 Catalog 条目
        catalog_.DropTable(stmt.table_name);
        return false;
    }

    return true;
}

bool Database::DropTable(std::string_view table_name) {
    // 1. Catalog：查表并删除逻辑条目
    const auto table_id = catalog_.FindTable(table_name);
    if (!table_id.has_value()) {
        return false;
    }

    if (!catalog_.DropTable(table_name)) {
        return false;
    }

    // 2. StorageManager：标记删除物理目录（tables/{id} -> tables/{id}_dropped）
    if (!storage_manager_.DropTable(*table_id)) {
        return false;
    }

    return true;
}

} // namespace simple_olap
