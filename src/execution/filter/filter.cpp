#include "filter.h"

#include "../batch_utils.h"
#include "../expression/vector_predicate_compiler.h"

namespace simple_olap {

void FilterOperator::Init() {
    child_->Init();

    // 谓词树只编译一次；之后每个 batch 只执行已绑定的 mask 运算。
    prepared_predicate_ = CompileVectorPredicate(*predicate_);
}

bool FilterOperator::Next(VectorBatch& batch) {
    while (child_->Next(batch)) {
        const uint32_t physical_count = batch.PhysicalSize();
        if (physical_count == 0) {
            continue;
        }

        // batch.selection 直接作为输入 mask（承载 Storage 下推 / 上级 Filter 的结果）。
        // result_mask_ ⊆ batch.selection 的不变式由 VectorPredicate 保证。
        prepared_predicate_->Evaluate(batch, physical_count, batch.selection(), result_mask_);

        if (result_mask_.Empty()) {
            continue;
        }

        if (result_mask_.IsAll()) {
            batch.SetIdentitySelection(physical_count);
        } else {
            batch.SetSelection(result_mask_);
        }
        return true;
    }

    return false;
}

} // namespace simple_olap
