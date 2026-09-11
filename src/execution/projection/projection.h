#pragma once

#include <memory>
#include <vector>

#include "../expression/exec_expression.h"
#include "../operator.h"
#include "../vector/vector.h"

namespace simple_olap {

class ProjectionOperator final : public Operator {
  public:
    ProjectionOperator(std::unique_ptr<Operator> child, std::vector<ExecExprPtr> expressions, BufferPool* buffer_pool)
        : child_(std::move(child)), expressions_(std::move(expressions)), input_(buffer_pool, true) {}

    void Init() override;
    bool Next(VectorBatch& output) override;

  private:
    bool AllDirectColumnRefs() const;
    bool ProduceViewProjection(VectorBatch& output);
    bool ProduceMaterializedProjection(VectorBatch& output);

    std::unique_ptr<Operator> child_;
    std::vector<ExecExprPtr> expressions_;
    VectorBatch input_;
};

} // namespace simple_olap
