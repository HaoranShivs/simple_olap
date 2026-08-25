#include "storage_manager.h"

namespace simple_olap
{
    StorageManager::StorageManager(TableId table_id, std::filesystem::path path, const TableSchema &schema,
                                   const std::vector<SegmentId> &sealed_segment_ids)
        : table_id_(table_id),
          table_path_(std::move(path)),
          schema_(schema),
          on_disk_segments_(sealed_segment_ids)
    {
        // 新 segment 的 id 从已有最大 id + 1 开始
        SegmentId next = 0;
        for (SegmentId id : sealed_segment_ids)
        {
            next = std::max(next, id + 1);
        }
        next_segment_id_ = next;
        CreateActiveSegment();
    }

    void StorageManager::Append(const DataChunk &input)
    {
        // 浅拷贝，共享底层缓冲；后续用 Slice 切块
        DataChunk chunk = input;
        while (chunk.size() > 0)
        {
            // 活跃 segment 的剩余容量
            uint32_t remaining = kMaxSegmentRowCount - active_segment_->row_count();

            if (chunk.size() <= remaining)
            {
                // 整个 chunk 放得下，直接写入
                active_segment_->Append(chunk);
                break;
            }

            // 剩余容量不够，切块：本 segment 先装前 remaining 行
            DataChunk rest = chunk.Slice(remaining);
            active_segment_->Append(chunk);

            // 封存当前 segment，开启新的活跃 segment
            SealActiveSegment();
            CreateActiveSegment();

            chunk = std::move(rest);
        }
    }

    void StorageManager::SealActiveSegment()
    {
        // 活跃 segment 已填满：不立即写盘，移入待刷盘 map，等待 Flush() 统一落盘
        sealed_segments_[active_segment_id_] = std::move(active_segment_);
    }

    void StorageManager::Flush()
    {
        // 将内存中所有待刷盘 segment 写盘，并登记到已落盘列表
        for (auto &entry : sealed_segments_)
        {
            entry.second->Flush(table_path_);
            on_disk_segments_.push_back(entry.first);
        }
        sealed_segments_.clear();
    }

    void StorageManager::CreateActiveSegment()
    {
        active_segment_id_ = next_segment_id_++;
        active_segment_ = std::make_unique<SegmentBuilder>(schema_);
    }

} // namespace simple_olap