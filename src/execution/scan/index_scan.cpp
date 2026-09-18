#include "index_scan.h"

#include <algorithm>
#include <stdexcept>

#include "../../catalog/catalog.h"
#include "../../storage/scan/row_filter.h"
#include "../../storage/storage_manager.h"
#include "../../storage/table/table_storage.h"
#include "../batch_utils.h"

namespace simple_olap {

void IndexScanOperator::Init() {
    if (ctx_ == nullptr || ctx_->catalog == nullptr || ctx_->storage_manager == nullptr) {
        throw std::runtime_error("IndexScanOperator: missing execution context");
    }

    const TableCatalogEntry* entry = ctx_->catalog->GetTable(table_id_);
    if (entry == nullptr) {
        throw std::runtime_error("IndexScanOperator: table not found: " + std::to_string(table_id_));
    }
    const TableSchema& schema = entry->schema;

    const KeySchema* key = FindKeySchema(schema, key_id_);
    if (key == nullptr) {
        throw std::runtime_error("IndexScanOperator: key not found: " + std::to_string(key_id_));
    }

    auto storage = ctx_->storage_manager->GetTable(table_id_, schema);
    if (storage == nullptr) {
        throw std::runtime_error("IndexScanOperator: table storage not found: " + std::to_string(table_id_));
    }
    table_ = storage.get();

    // 1. 编码查找键并点查可见行位置（只返回已落盘、与 SeqScan 可见性一致的行）
    const EncodedKey encoded = KeyEncoder::Encode(schema, *key, lookup_values_);
    std::vector<RowLocation> locations = table_->Lookup(key_id_, encoded);

    // 2. 按 segment 分组：ReadRows 以 segment 为单位
    std::sort(locations.begin(), locations.end(), [](const RowLocation& a, const RowLocation& b) {
        if (a.segment_id != b.segment_id) {
            return a.segment_id < b.segment_id;
        }
        return a.row_offset < b.row_offset;
    });

    groups_.clear();
    for (const RowLocation& location : locations) {
        if (groups_.empty() || groups_.back().segment_id != location.segment_id) {
            groups_.push_back(SegmentRows{location.segment_id, {}});
        }
        groups_.back().row_offsets.push_back(location.row_offset);
    }
    next_group_ = 0;
    next_offset_ = 0;

    // 3. residual predicates 准备一次，之后每个 batch 复用
    if (!residual_predicates_.empty()) {
        ScanOptions options;
        options.columns = columns_;
        options.predicates = residual_predicates_;
        prepared_ = std::make_shared<const PreparedScanPredicates>(PreparedScanPredicates::Build(options, schema));
    }
}

bool IndexScanOperator::Next(VectorBatch& batch) {
    ClearBatch(batch);

    if (table_ == nullptr) {
        throw std::runtime_error("IndexScanOperator::Next called before Init");
    }

    while (next_group_ < groups_.size()) {
        const SegmentRows& group = groups_[next_group_];
        const size_t remaining = group.row_offsets.size() - next_offset_;
        if (remaining == 0) {
            ++next_group_;
            next_offset_ = 0;
            continue;
        }

        // 一个批次最多 BATCH_SIZE 行
        const uint32_t count = static_cast<uint32_t>(std::min<size_t>(VectorBatch::BATCH_SIZE, remaining));
        const std::vector<uint32_t> offsets(group.row_offsets.begin() + next_offset_,
                                            group.row_offsets.begin() + next_offset_ + count);
        next_offset_ += count;

        if (!table_->ReadRows(group.segment_id, offsets, columns_, batch)) {
            throw std::runtime_error("IndexScanOperator: failed to read rows from segment " +
                                     std::to_string(group.segment_id));
        }

        // residual predicates：命中行已是精确行集，全部逐行精确判断
        if (prepared_ != nullptr && !prepared_->empty()) {
            std::vector<uint8_t> row_filter_mask(prepared_->size(), 1);
            StorageRowFilter::Apply(*prepared_, row_filter_mask, batch);
        }

        if (batch.size == 0) {
            continue; // 本批全部被 residual 过滤，继续读下一批
        }

        return true;
    }

    return false;
}

} // namespace simple_olap
