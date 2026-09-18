#pragma once

#include "../../catalog/catalog.h"
#include "physical_plan.h"

namespace simple_olap {

// LogicalPlan -> PhysicalPlan。
// 当前核心版本不做 cost-based search：
//   Scan -> 完整等值主键 / 完整等值二级键命中时生成 IndexScan，否则 SeqScan；
//   Aggregate -> HashAggregate。
// 使用哪个索引属于物理执行策略，因此判断放在这里而不是 LogicalPlan。
class PhysicalPlanner {
  public:
    // catalog 用于读取键定义（schema / primary_key / secondary_keys），
    // 生命周期必须覆盖本对象。
    explicit PhysicalPlanner(const Catalog& catalog) : catalog_(&catalog) {}

    PhysicalPlanPtr CreatePhysicalPlan(const LogicalPlan& logical) const;

  private:
    // Access Path Selection：为 LogicalScan 选择 SeqScan 或 IndexScan
    PhysicalPlanPtr PlanScan(const LogicalScan& scan) const;

    const Catalog* catalog_ = nullptr;
};

} // namespace simple_olap
