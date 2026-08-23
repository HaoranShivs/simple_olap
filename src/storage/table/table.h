#pragma once

#include "storage_manager.h"
#include "../datachunk.h"

namespace simple_olap
{
    struct ScanOptions
    {
        uint64_t start_row = 0;        // 起始行
        uint64_t end_row = UINT64_MAX; // 结束行
        std::vector<ColumnId> columns; // 需要读取的列，空表示全部
        // 过滤条件
        // codition
    };

    class Table
    {
    public:
        // 禁止拷贝
        Table(const Table &) = delete;
        Table &operator=(const Table &) = delete;

        // 允许移动
        Table(Table &&) noexcept = default;
        Table &operator=(Table &&) noexcept = default;

        // 创建新表
        static std::unique_ptr<Table> Create(const TableMeta &metadata, const std::filesystem::path &root_dir);

        // 从硬盘引入表
        static std::unique_ptr<Table> Open(TableId id, const std::filesystem::path &root_dir);

        // 将表写回硬盘
        bool Flush();

        // 获取表头
        const TableSchema &GetSchema() const;

        // 获取表的scaner
        std::unique_ptr<class TableScaner> Scan(const ScanOptions &options) const;

        // 将新的datachunk增加到表的数据中
        bool Append(const DataChunk &datachunk);

    private:
        Table(const TableMeta *metadata, StorageManager *storage);

        TableMeta metadata_;
        std::unique_ptr<StorageManager> storage_;;
    };

} // namespace simple_olap