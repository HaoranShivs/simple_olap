#pragma once

#include <memory>
#include <vector>
#include "storage_manager.h"
#include "../datachunk.h"
#include "../../execution/vector/vector.h"

namespace simple_olap
{
    // 扫描游标：记录当前读取位置
    struct ScanCursor
    {
        SegmentId segment_id = 0;   // 当前 segment id
        uint32_t offset_in_segment = 0; // 在当前 segment 内的行偏移
    };

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