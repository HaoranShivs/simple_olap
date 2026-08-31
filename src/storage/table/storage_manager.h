#pragma once

#include <algorithm>
#include <memory>
#include <unordered_map>
#include "../datastructs.h"
#include "../segment/segment.h"
#include "../test_statement.h"


namespace simple_olap
{
    struct ScanCursor;
    class VectorBatch;
    class TableScan;

    class StorageManager
    {
    public:
        StorageManager(TableId table_id, std::filesystem::path path, const TableMeta &metadata);

        // 从table的多个segment中读取一个提取数据的VectorBatch
        // 核心步骤包括：1. 根据cursor中的segment_id选定segment，并传入offset_in_segment。2.根据返回的output的.size()，如果等于0，则cursor中的segment_id+=1， offset_in_segment=0，然后继续循环读取；如果大于0，则按照output.size()给offset_in_segment加上，退出循环。3. 根据request中where 的条件，过滤出有效的output。4.返回
        // @param request，所有扫描需要的信息
        // @param cursor，记录扫描到了哪个segment，哪行
        // @param output，用来保存提取的结果
        bool GetVectorBatch(const SelectTargetStatement &request, ScanCursor &cursor, VectorBatch &output);

        void Append(const DataChunk &input);

        void Flush();

        // segment 总数（已落盘 + 内存中待刷盘）
        size_t segment_count() const noexcept
        {
            return table_meta_.segment_ids.size() + sealed_segments_.size();
        }

        // 表元数据（含已落盘 segment id 列表），供上层 Table 同步/持久化
        const TableMeta &table_meta() const noexcept { return table_meta_; }

        // 表数据目录
        const std::filesystem::path &path() const noexcept { return table_path_; }

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

    private:
        void SealActiveSegment();

        void CreateActiveSegment();

        // identity：表元数据；已落盘 segment 的 id 直接记录在 table_meta_.segment_ids 中
        TableMeta table_meta_;

        std::filesystem::path table_path_;

        // 内存中待刷盘的 segment：id -> 填满的 SegmentBuilder
        std::unordered_map<SegmentId, std::unique_ptr<SegmentBuilder>> sealed_segments_;

        // 已落盘 segment 的只读视图缓存：id -> SegmentReader（懒加载，避免重复 mmap）
        std::unordered_map<SegmentId, std::unique_ptr<SegmentReader>> reader_cache_;

        // 获取（必要时打开）指定 id 的 SegmentReader；失败返回 nullptr
        SegmentReader *GetSegmentReader(SegmentId id);

        // append state
        SegmentId active_segment_id_;

        SegmentId next_segment_id_;

        std::unique_ptr<SegmentBuilder> active_segment_;

        // // 根据 segment_id 查找 SegmentBuilder（仅查内存中的 sealed 和 active）
        // SegmentBuilder* FindSegmentById(SegmentId id) noexcept;
    };

} // namespace simple_olap
