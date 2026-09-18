#include "primary_key_index.h"

#include <unordered_set>

namespace simple_olap {

bool PrimaryKeyIndex::ValidateBatch(const std::vector<EncodedKey>& keys) const {
    std::unordered_set<EncodedKey, EncodedKeyHash> batch_keys;
    batch_keys.reserve(keys.size());

    for (const auto& key : keys) {
        // 批内重复
        if (!batch_keys.insert(key).second) {
            return false;
        }
        // 与已插入数据重复（含未落盘、尚未对查询可见的键）
        if (entries_.count(key) > 0) {
            return false;
        }
    }

    return true;
}

void PrimaryKeyIndex::Insert(EncodedKey key, RowLocation location, bool visible) {
    // 唯一性已由 ValidateBatch 保证；同键重复插入不应发生
    entries_[std::move(key)] = IndexEntry{location, visible};
}

std::optional<RowLocation> PrimaryKeyIndex::Lookup(const EncodedKey& key) const {
    const auto it = entries_.find(key);
    if (it == entries_.end() || !it->second.visible) {
        return std::nullopt;
    }
    return it->second.location;
}

void PrimaryKeyIndex::MarkSegmentVisible(SegmentId segment_id) {
    // 每个 segment 只落盘一次，遍历一次标记即可；flush 不是热路径
    for (auto& entry : entries_) {
        if (entry.second.location.segment_id == segment_id) {
            entry.second.visible = true;
        }
    }
}

} // namespace simple_olap
