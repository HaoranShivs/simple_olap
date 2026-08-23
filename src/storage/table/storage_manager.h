#pragma once

#include <memory>
#include "../datastructs.h"
#include "../segment/segment.h"


namespace simple_olap
{

    class StorageManager
    {
    public:
        StorageManager(TableId table_id, std::filesystem::path path);

        std::unique_ptr<Scaner> Scan(const ScanRequest &request) const;

        void Append(const DataChunk &input);

        void Flush();

        size_t segment_count() const noexcept
        {
            return sealed_segments_.size();
        }

    private:
        void SealActiveSegment();

        void CreateActiveSegment();

        // identity
        TableId table_id_;

        std::filesystem::path table_path_;

        std::vector<SegmentMeta> sealed_segments_;

        // append state
        SegmentId active_segment_id_;

        SegmentId next_segment_id_;

        std::unique_ptr<SegmentBuilder> active_segment_;
    };

} // namespace simple_olap
