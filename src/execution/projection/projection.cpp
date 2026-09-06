#include "projection.h"

#include <stdexcept>

#include "../batch_utils.h"

namespace simple_olap {

void ProjectionOperator::Init() {
    child_->Init();
    ClearBatch(input_);
}

bool ProjectionOperator::AllDirectColumnRefs() const {
    for (const auto &expr : expressions_) {
        if (expr->GetType() != ExecExpression::Type::COLUMN_REF) {
            return false;
        }
    }
    return true;
}

bool ProjectionOperator::ProduceViewProjection(VectorBatch &output) {
    ClearBatch(output);

    for (const auto &expr : expressions_) {
        const auto &ref = static_cast<const ExecColumnRef &>(*expr);
        const uint32_t slot = ref.GetInputSlot();
        if (slot >= input_.columns.size()) {
            throw std::runtime_error("ProjectionOperator: input slot out of range");
        }

        const auto &src = input_.columns[slot];
        output.AddColumn(src.type);
        output.columns.back().CopyFrom(src.buffer, src.count, true);
    }

    output.size = ActiveRowCount(input_);
    output.sel_vector = input_.sel_vector;
    return true;
}

bool ProjectionOperator::ProduceMaterializedProjection(VectorBatch &output) {
    ClearBatch(output);

    const uint32_t active = ActiveRowCount(input_);
    for (const auto &expr : expressions_) {
        output.AddColumn(expr->GetReturnType());
        output.columns.back().Resize(active);
    }

    for (uint32_t logical = 0; logical < active; ++logical) {
        const uint32_t physical = ActiveRowIndex(input_, logical);
        for (uint32_t col = 0; col < static_cast<uint32_t>(expressions_.size()); ++col) {
            const ExecValue value = expressions_[col]->Eval(input_, physical);
            WriteExecValue(output.columns[col], logical, value);
        }
    }

    output.size = active;
    output.sel_vector.clear();
    return true;
}

bool ProjectionOperator::Next(VectorBatch &output) {
    if (!child_->Next(input_)) {
        ClearBatch(output);
        return false;
    }

    if (AllDirectColumnRefs()) {
        return ProduceViewProjection(output);
    }
    return ProduceMaterializedProjection(output);
}

} // namespace simple_olap
