#include "seq_scan.h"

#include <stdexcept>

#include "../../catalog/catalog.h"
#include "../../storage/storage_manager.h"
#include "../batch_utils.h"

namespace simple_olap {

void SeqScanOperator::Init() {
    if (ctx_ == nullptr || ctx_->catalog == nullptr || ctx_->storage_manager == nullptr) {
        throw std::runtime_error("SeqScanOperator: missing execution context");
    }

    // bind 已把表名解析成 table_id；scan 阶段不再需要 Catalog 参与，
    // 只通过 StorageManager 拿到物理表对象（schema 从 Catalog 传入）
    const TableCatalogEntry* entry = ctx_->catalog->GetTable(table_id_);
    if (entry == nullptr) {
        throw std::runtime_error("SeqScanOperator: table not found: " + std::to_string(table_id_));
    }

    auto storage = ctx_->storage_manager->GetTable(table_id_, entry->schema);
    if (storage == nullptr) {
        throw std::runtime_error("SeqScanOperator: table storage not found: " + std::to_string(table_id_));
    }
    table_ = storage.get();

    cursor_ = ScanCursor{};
}

bool SeqScanOperator::Next(VectorBatch& batch) {
    if (table_ == nullptr) {
        throw std::runtime_error("SeqScanOperator::Next called before Init");
    }

    ClearBatch(batch);

    // 跨 segment 推进游标并提取数据（zone map pruning + row filter 在存储层完成）
    return table_->Scan(options_, cursor_, batch);
}

} // namespace simple_olap
