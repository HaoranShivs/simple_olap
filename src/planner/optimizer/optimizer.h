#pragma once

#include <memory>
#include <vector>

#include "../logical_plan/logical_plan.h"

namespace simple_olap {

class OptimizerRule {
public:
    virtual ~OptimizerRule() = default;
    virtual bool Apply(LogicalPlanPtr &root) const = 0;
};

/// Filter(simple column cmp literal) -> Scan.pushed_predicate.
/// 只下推 storage 当前能完整执行的简单谓词；AND/OR 等复杂谓词保留在 Filter。
class PredicatePushdownRule final : public OptimizerRule {
public:
    bool Apply(LogicalPlanPtr &root) const override;
};

/// 从 Project/Aggregate/Filter 表达式收集列引用，写入 Scan.required_columns。
class ColumnPruningRule final : public OptimizerRule {
public:
    bool Apply(LogicalPlanPtr &root) const override;
};

class Optimizer {
public:
    Optimizer();
    void Optimize(LogicalPlanPtr &root) const;

private:
    std::vector<std::unique_ptr<OptimizerRule>> rules_;
};

} // namespace simple_olap
