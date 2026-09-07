#pragma once

#include <filesystem>

#include "../catalog/catalog.h"
#include "../parallel/thread_pool/thread_pool.h"
#include "../storage/storage_manager.h"

namespace simple_olap {

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