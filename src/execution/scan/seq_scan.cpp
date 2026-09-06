#include "seq_scan.h"

#include <stdexcept>

#include "../../storage/catalog.h"
#include "../../storage/table/table.h"
#include "../batch_utils.h"

namespace simple_olap {

void SeqScanOperator::Init() {
    if (ctx_ == nullptr || ctx_->catalog == nullptr) {
        throw std::runtime_error("SeqScanOperator: missing execution context/catalog");
    }

    table_ = ctx_->catalog->GetTable(table_id_);
    if (table_ == nullptr) {
        throw std::runtime_error("SeqScanOperator: table not found: " +
                                 std::to_string(table_id_));
    }

    cursor_ = ScanCursor{};
}

bool SeqScanOperator::Next(VectorBatch &batch) {
    if (table_ == nullptr) {
        throw std::runtime_error("SeqScanOperator::Next called before Init");
    }

    ClearBatch(batch);

    // Required storage API:
    //   bool Table::GetVectorBatch(const ScanOptions&, ScanCursor&, VectorBatch&);
    // See REQUIRED_STORAGE_CHANGES.md in this package.
    return table_->GetVectorBatch(options_, cursor_, batch);
}

} // namespace simple_olap
