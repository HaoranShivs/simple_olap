#include "hash_aggregate.h"

#include <stdexcept>

namespace simple_olap {

void HashAggregateOperator::Init() {
    if (child_) {
        child_->Init();
    }
    consumed_ = false;
}

bool HashAggregateOperator::Next(VectorBatch& output) {
    if (!consumed_) {
        consumed_ = true;
        while (child_->Next(input_)) {
            state_.Consume(input_);
        }
    }
    return state_.NextResult(output);
}

} // namespace simple_olap
