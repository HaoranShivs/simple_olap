#include "secondary_key_index.h"

namespace simple_olap {

void SecondaryKeyIndex::Insert(EncodedKey key, RowLocation location, bool visible) {
    entries_[std::move(key)].push_back(IndexEntry{location, visible});
}

std::vector<RowLocation> SecondaryKeyIndex::LookupEqual(const EncodedKey& key) const {
    std::vector<RowLocation> result;

    const auto it = entries_.find(key);
    if (it == entries_.end()) {
        return result;
    }

    result.reserve(it->second.size());
    for (const auto& entry : it->second) {
        if (entry.visible) {
            result.push_back(entry.location);
        }
    }
    return result;
}

void SecondaryKeyIndex::MarkSegmentVisible(SegmentId segment_id) {
    for (auto& bucket : entries_) {
        for (auto& entry : bucket.second) {
            if (entry.location.segment_id == segment_id) {
                entry.visible = true;
            }
        }
    }
}

} // namespace simple_olap
