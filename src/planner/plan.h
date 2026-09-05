#pragma once

#include "planner.h"
#include "optimizer/optimizer.h"
#include "physical_plan/physical_planner.h"

namespace simple_olap {

/// 最小 façade：方便 main/test 一次完成 logical -> optimize -> physical。
class PlanPipeline {
public:
    PhysicalPlanPtr Build(const BoundStatement &statement,
                          std::string *logical_explain = nullptr) const {
        auto logical = planner_.CreateLogicalPlan(statement);
        optimizer_.Optimize(logical);
        if (logical_explain) *logical_explain = logical->ToString();
        return physical_planner_.CreatePhysicalPlan(*logical);
    }

private:
    Planner planner_;
    Optimizer optimizer_;
    PhysicalPlanner physical_planner_;
};

} // namespace simple_olap
