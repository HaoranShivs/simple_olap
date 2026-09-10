#pragma once

#include "physical_plan.h"

namespace simple_olap {

// LogicalPlan -> PhysicalPlan。
// 当前核心版本不做 cost-based search：Scan->SeqScan，Aggregate->HashAggregate。
class PhysicalPlanner {
  public:
    PhysicalPlanPtr CreatePhysicalPlan(const LogicalPlan& logical) const;
};

} // namespace simple_olap
