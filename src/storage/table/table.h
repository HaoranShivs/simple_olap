#pragma once

#include <memory>
#include <vector>
#include "storage_manager.h"
#include "../datachunk.h"
#include "../../execution/vector/vector.h"


namespace simple_olap
{
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

        // 将新的datachunk增加到表的数据中
        bool Append(const DataChunk &datachunk);

        // 从table中获取一个提取数据的VectorBatch
        // @param request，所有扫描需要的信息
        // @param cursor，记录扫描到了哪个segment，哪行
        // @param output，用来保存提取的结果
        bool GetVectorBatch(const SelectTargetStatement &request, ScanCursor &cursor, VectorBatch &output);

    private:
        Table(TableMeta metadata, std::unique_ptr<StorageManager> storage);

        TableMeta metadata_;
        std::unique_ptr<StorageManager> storage_;
    };

} // namespace simple_olap