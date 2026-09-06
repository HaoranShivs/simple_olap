#include "../planner/plan.h"
#include "operator.h"
#include <memory>

namespace simple_olap {
class ExecutorBuilder {
  public:
    explicit ExecutorBuilder(ExecutionContext* ctx) : ctx_(ctx) {}

    std::unique_ptr<Operator> Build(const PhysicalPlan& plan);

  private:
    std::unique_ptr<Operator> BuildSeqScan(const PhysicalSeqScan& plan);

    std::unique_ptr<Operator> BuildFilter(const PhysicalFilter& plan);

    std::unique_ptr<Operator> BuildProject(const PhysicalProject& plan);

    std::unique_ptr<Operator> BuildAggregate(const PhysicalHashAggregate& plan);

  private:
    ExecutionContext* ctx_;
};
} // namespace simple_olap
