#include "filter.h"

#include "../batch_utils.h"
#include "../expression/vector_predicate_compiler.h"

namespace simple_olap {

void FilterOperator::Init() {
    child_->Init();

    selection_.clear();
    selection_.reserve(VectorBatch::BATCH_SIZE);

    // 谓词树只编译一次；之后每个 batch 只执行已绑定的 mask 运算。
    prepared_predicate_ = CompileVectorPredicate(*predicate_);
}

bool FilterOperator::Next(VectorBatch& batch) {
    while (child_->Next(batch)) {
        const uint32_t physical_count = PhysicalRowCount(batch);
        if (physical_count == 0) {
            continue;
        }

        // input_mask 表达“子算子已经选中的物理行”。
        // 空 sel_vector 表示全选；非空 sel_vector 表示下推过滤后的子集。
        if (batch.sel_vector.empty()) {
            input_mask_.SetAll(physical_count);
        } else {
            input_mask_.FromSelectionVector(batch.sel_vector, physical_count);
        }

        prepared_predicate_->Evaluate(batch, physical_count, input_mask_, result_mask_);

        if (result_mask_.Empty()) {
            continue;
        }

        // result_mask_ ⊆ input_mask_，因此 IsAll() 为真时等价于原先的
        // IsIdentitySelection(selection_, physical_count)。
        if (result_mask_.IsAll()) {
            batch.sel_vector.clear();
            batch.size = physical_count;
        } else {
            result_mask_.ToSelectionVector(selection_);
            batch.size = static_cast<uint32_t>(selection_.size());
            batch.sel_vector = selection_;
        }
        return true;
    }

    return false;
}

} // namespace simple_olap
