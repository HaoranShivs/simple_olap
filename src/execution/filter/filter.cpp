#include "filter.h"

#include "../batch_utils.h"

namespace simple_olap {

void FilterOperator::Init() {
    child_->Init();
    selection_.clear();
    selection_.reserve(VectorBatch::BATCH_SIZE);
}

bool FilterOperator::Next(VectorBatch &batch) {
    while (child_->Next(batch)) {
        const uint32_t active = ActiveRowCount(batch);
        selection_.clear();

        for (uint32_t i = 0; i < active; ++i) {
            const uint32_t row = ActiveRowIndex(batch, i);
            if (ExecValueAsBool(predicate_->Eval(batch, row))) {
                selection_.push_back(row);
            }
        }

        if (selection_.empty()) {
            continue;
        }

        const uint32_t physical_count = PhysicalRowCount(batch);
        batch.size = static_cast<uint32_t>(selection_.size());
        if (IsIdentitySelection(selection_, physical_count)) {
            batch.sel_vector.clear();
        } else {
            batch.sel_vector = selection_;
        }
        return true;
    }

    return false;
}

} // namespace simple_olap
