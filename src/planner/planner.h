#pragma once

#include "logical_plan/logical_plan.h"

namespace simple_olap {

// BoundStatement -> LogicalPlan。
// 语义绑定（表名/列名解析、类型检查）必须在进入 Planner 前完成。
class Planner {
  public:
    LogicalPlanPtr CreateLogicalPlan(const BoundStatement& statement) const;

  private:
    LogicalPlanPtr PlanSelect(const BoundSelectStatement& statement) const;
    LogicalPlanPtr PlanInsert(const BoundInsertStatement& statement) const;
    LogicalPlanPtr PlanCreateTable(const BoundCreateTableStatement& statement) const;
};

} // namespace simple_olap
