#include "storage_manager.h"

#include <system_error>

namespace simple_olap {

StorageManager::StorageManager(std::filesystem::path db_path, BufferPool* buffer_pool)
    : root_path_(std::move(db_path)), buffer_pool_(buffer_pool) {
    // 数据库根目录必须已存在（由 Database 负责创建）；tables/ 子目录按需创建
}

StorageManager::~StorageManager() {
    // 兜底：把所有已打开表的内存 segment 落盘
    Flush();
}

std::filesystem::path StorageManager::TablesRoot() const {
    return root_path_ / "tables";
}

std::shared_ptr<TableStorage> StorageManager::GetTable(TableId table_id, const TableSchema& schema) {
    // 1. 已打开则直接复用
    const auto it = tables_.find(table_id);
    if (it != tables_.end()) {
        return it->second;
    }

    // 2. 从硬盘打开（table.meta 中记录 segment 布局）；
    //    schema 由调用方从 Catalog 取出后传入（StorageManager 不感知 Catalog）
    auto storage = TableStorage::Open(table_id, schema, TablesRoot(), buffer_pool_);
    if (storage == nullptr) {
        return nullptr;
    }

    auto shared = std::shared_ptr<TableStorage>(std::move(storage));
    tables_[table_id] = shared;
    return shared;
}

std::shared_ptr<TableStorage> StorageManager::CreateTable(TableId table_id, const TableSchema& schema) {
    // 已存在同 id 的打开表则拒绝（Catalog 侧负责重名检查）
    if (tables_.count(table_id) > 0) {
        return nullptr;
    }

    auto storage = TableStorage::Create(table_id, schema, TablesRoot(), buffer_pool_);
    if (storage == nullptr) {
        return nullptr;
    }

    auto shared = std::shared_ptr<TableStorage>(std::move(storage));
    tables_[table_id] = shared;
    return shared;
}

bool StorageManager::DropTable(TableId table_id) {
    // 1. 从缓存移除（TableStorage 析构时会兜底落盘）
    tables_.erase(table_id);

    // 2. 保守删除：不直接 remove_all，而是把物理目录改名为
    //    tables/{table_id}_dropped，留给专门的处理程序按计划清理。
    //    这样即使 drop 过程中进程崩溃，数据也只是"被标记"而非丢失。
    std::error_code ec;
    const std::filesystem::path table_path = TablesRoot() / std::to_string(table_id);
    if (!std::filesystem::exists(table_path, ec)) {
        return !ec;
    }

    const std::filesystem::path dropped_path = TablesRoot() / (std::to_string(table_id) + "_dropped");
    // 若上次 drop 留下了同名残留，先清掉标记目录（此时才允许删除）
    if (std::filesystem::exists(dropped_path, ec)) {
        std::filesystem::remove_all(dropped_path, ec);
        if (ec) {
            return false;
        }
    }

    std::filesystem::rename(table_path, dropped_path, ec);
    return !ec;
}

void StorageManager::Flush() {
    for (auto& entry : tables_) {
        entry.second->Flush();
    }
}

} // namespace simple_olap
