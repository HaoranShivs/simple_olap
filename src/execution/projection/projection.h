#pragma once

#include <memory>
#include <vector>

#include "../expression/exec_expression.h"
#include "../operator.h"

namespace simple_olap {

class ProjectionOperator final : public Operator {
public:
    ProjectionOperator(std::unique_ptr<Operator> child,
                       std::vector<ExecExprPtr> expressions)
        : child_(std::move(child)), expressions_(std::move(expressions)) {}

    void Init() override;
    bool Next(VectorBatch &output) override;

private:
    bool AllDirectColumnRefs() const;
    bool ProduceViewProjection(VectorBatch &output);
    bool ProduceMaterializedProjection(VectorBatch &output);

    std::unique_ptr<Operator> child_;
    std::vector<ExecExprPtr> expressions_;
    VectorBatch input_{true};
};

} // namespace simple_olap
