#pragma once

#include <memory>
#include <unordered_map>
#include "../datastructs.h"
#include "../segment/segment.h"
#include "../test_statement.h"


namespace simple_olap
{
    class TableScan;

    class StorageManager
    {
    public:
        // sealed_segment_ids: 已写入硬盘的 segment id 列表（新建表传空列表）
        StorageManager(TableId table_id, std::filesystem::path path, const TableSchema &schema,
                       const std::vector<SegmentId> &sealed_segment_ids = {});

        // StorageManager::Scan：接收 id-based 查询请求，返回 TableScan
        std::unique_ptr<class TableScan> Scan(const SelectTargetStatement &request) const;

        void Append(const DataChunk &input);

        void Flush();

        // segment 总数（已落盘 + 内存中待刷盘）
        size_t segment_count() const noexcept
        {
            return on_disk_segments_.size() + sealed_segments_.size();
        }

        // 已写入硬盘的 segment id 列表
        const std::vector<SegmentId> &on_disk_segments() const noexcept
        {
            return on_disk_segments_;
        }

        // 内存中待刷盘的 segment id 列表
        std::vector<SegmentId> sealed_segment_ids() const
        {
            std::vector<SegmentId> ids;
            ids.reserve(sealed_segments_.size());
            for (const auto &entry : sealed_segments_)
            {
                ids.push_back(entry.first);
            }
            return ids;
        }

        // 表数据目录
        const std::filesystem::path &path() const noexcept
        {
            return table_path_;
        }

    private:
        void SealActiveSegment();

        void CreateActiveSegment();

        // identity
        TableId table_id_;

        std::filesystem::path table_path_;

        TableSchema schema_;

        

        // 已写入硬盘的 segment id 列表（构造时由 table.meta 载入）
        std::vector<SegmentId> on_disk_segments_;

        // 内存中待刷盘的 segment：id -> 填满的 SegmentBuilder
        std::unordered_map<SegmentId, std::unique_ptr<SegmentBuilder>> sealed_segments_;

        // append state
        SegmentId active_segment_id_;

        SegmentId next_segment_id_;

        std::unique_ptr<SegmentBuilder> active_segment_;
    };

} // namespace simple_olap
