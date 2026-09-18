#include "table_index_manager.h"

#include <algorithm>

#include "../../execution/vector/vector.h"

namespace simple_olap {

TableIndexManager::TableIndexManager(const TableSchema& schema) : schema_(&schema) {
    if (schema.primary_key.has_value()) {
        primary_index_ = std::make_unique<PrimaryKeyIndex>();
    }

    secondary_indexes_.reserve(schema.secondary_keys.size());
    for (size_t i = 0; i < schema.secondary_keys.size(); ++i) {
        secondary_indexes_.push_back(std::make_unique<SecondaryKeyIndex>());
    }

    // 键列并集（去重升序）：重建索引时按这份列清单读取 segment
    for (const auto& key : schema.secondary_keys) {
        key_columns_.insert(key_columns_.end(), key.columns.begin(), key.columns.end());
    }
    if (schema.primary_key.has_value()) {
        key_columns_.insert(key_columns_.end(), schema.primary_key->columns.begin(), schema.primary_key->columns.end());
    }
    std::sort(key_columns_.begin(), key_columns_.end());
    key_columns_.erase(std::unique(key_columns_.begin(), key_columns_.end()), key_columns_.end());
}

// ---------- 唯一性 ----------

bool TableIndexManager::ValidatePrimaryKey(const DataChunk& chunk) const {
    if (primary_index_ == nullptr) {
        return true;
    }

    const KeySchema& key = *schema_->primary_key;
    std::vector<EncodedKey> keys;
    keys.reserve(chunk.size());
    for (uint32_t row = 0; row < chunk.size(); ++row) {
        keys.push_back(KeyEncoder::Encode(*schema_, key, chunk, row));
    }

    return primary_index_->ValidateBatch(keys);
}

// ---------- 索引维护 ----------

void TableIndexManager::OnAppend(const DataChunk& chunk, const std::vector<RowLocation>& locations) {
    const uint32_t row_count = static_cast<uint32_t>(locations.size());

    if (primary_index_ != nullptr) {
        const KeySchema& key = *schema_->primary_key;
        for (uint32_t row = 0; row < row_count; ++row) {
            primary_index_->Insert(KeyEncoder::Encode(*schema_, key, chunk, row), locations[row], false);
        }
    }

    for (size_t i = 0; i < secondary_indexes_.size(); ++i) {
        const KeySchema& key = schema_->secondary_keys[i];
        for (uint32_t row = 0; row < row_count; ++row) {
            secondary_indexes_[i]->Insert(KeyEncoder::Encode(*schema_, key, chunk, row), locations[row], false);
        }
    }
}

void TableIndexManager::RebuildFromBatch(const std::vector<ColumnId>& batch_columns, const VectorBatch& batch,
                                         const std::vector<RowLocation>& locations) {
    const uint32_t row_count = static_cast<uint32_t>(locations.size());

    if (primary_index_ != nullptr) {
        const KeySchema& key = *schema_->primary_key;
        for (uint32_t row = 0; row < row_count; ++row) {
            primary_index_->Insert(KeyEncoder::Encode(*schema_, key, batch_columns, batch, row), locations[row], true);
        }
    }

    for (size_t i = 0; i < secondary_indexes_.size(); ++i) {
        const KeySchema& key = schema_->secondary_keys[i];
        for (uint32_t row = 0; row < row_count; ++row) {
            secondary_indexes_[i]->Insert(KeyEncoder::Encode(*schema_, key, batch_columns, batch, row), locations[row],
                                          true);
        }
    }
}

void TableIndexManager::MarkSegmentsVisible(const std::vector<SegmentId>& segment_ids) {
    for (SegmentId segment_id : segment_ids) {
        if (primary_index_ != nullptr) {
            primary_index_->MarkSegmentVisible(segment_id);
        }
        for (auto& index : secondary_indexes_) {
            index->MarkSegmentVisible(segment_id);
        }
    }
}

// ---------- 查找 ----------

std::vector<RowLocation> TableIndexManager::Lookup(KeyId key_id, const EncodedKey& key) const {
    std::vector<RowLocation> result;

    if (primary_index_ != nullptr && schema_->primary_key->key_id == key_id) {
        const auto location = primary_index_->Lookup(key);
        if (location.has_value()) {
            result.push_back(*location);
        }
        return result;
    }

    for (size_t i = 0; i < schema_->secondary_keys.size(); ++i) {
        if (schema_->secondary_keys[i].key_id == key_id) {
            return secondary_indexes_[i]->LookupEqual(key);
        }
    }

    return result;
}

} // namespace simple_olap
