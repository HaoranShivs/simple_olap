#pragma once

#include <memory>

#include "../expression/exec_expression.h"
#include "../operator.h"

namespace simple_olap {

class FilterOperator final : public Operator {
public:
    FilterOperator(std::unique_ptr<Operator> child, ExecExprPtr predicate)
        : child_(std::move(child)), predicate_(std::move(predicate)) {}

    void Init() override;
    bool Next(VectorBatch &batch) override;

private:
    std::unique_ptr<Operator> child_;
    ExecExprPtr predicate_;
    std::vector<uint32_t> selection_;
};

} // namespace simple_olap
