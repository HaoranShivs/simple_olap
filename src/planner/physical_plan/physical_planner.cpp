#include "physical_planner.h"

#include <stdexcept>

namespace simple_olap {

namespace {

std::vector<NamedPlanExpr> CloneNamed(const std::vector<NamedPlanExpr> &src) {
    std::vector<NamedPlanExpr> out;
    out.reserve(src.size());
    for (const auto &item : src) out.emplace_back(item.expr->Clone(), item.alias);
    return out;
}

std::vector<PlanExprPtr> CloneExprs(const std::vector<PlanExprPtr> &src) {
    std::vector<PlanExprPtr> out;
    out.reserve(src.size());
    for (const auto &expr : src) out.push_back(expr->Clone());
    return out;
}

std::vector<std::vector<PlanExprPtr>> CloneRows(const std::vector<std::vector<PlanExprPtr>> &src) {
    std::vector<std::vector<PlanExprPtr>> out;
    out.reserve(src.size());
    for (const auto &src_row : src) {
        std::vector<PlanExprPtr> row;
        row.reserve(src_row.size());
        for (const auto &expr : src_row) row.push_back(expr->Clone());
        out.push_back(std::move(row));
    }
    return out;
}

} // namespace

PhysicalPlanPtr PhysicalPlanner::CreatePhysicalPlan(const LogicalPlan &logical) const {
    switch (logical.GetType()) {
    case LogicalPlan::Type::SCAN: {
        const auto &scan = static_cast<const LogicalScan &>(logical);
        return std::make_unique<PhysicalSeqScan>(
            scan.GetTableOid(), scan.GetRequiredColumns(), scan.GetPushedPredicate());
    }
    case LogicalPlan::Type::FILTER: {
        const auto &filter = static_cast<const LogicalFilter &>(logical);
        return std::make_unique<PhysicalFilter>(
            filter.GetPredicate().Clone(), CreatePhysicalPlan(filter.GetChild()));
    }
    case LogicalPlan::Type::PROJECT: {
        const auto &project = static_cast<const LogicalProject &>(logical);
        return std::make_unique<PhysicalProject>(
            CloneNamed(project.GetOutputs()), CreatePhysicalPlan(project.GetChild()));
    }
    case LogicalPlan::Type::AGGREGATE: {
        const auto &aggregate = static_cast<const LogicalAggregate &>(logical);
        return std::make_unique<PhysicalHashAggregate>(
            CloneExprs(aggregate.GetGroupBy()), CloneNamed(aggregate.GetOutputs()),
            CreatePhysicalPlan(aggregate.GetChild()));
    }
    case LogicalPlan::Type::INSERT: {
        const auto &insert = static_cast<const LogicalInsert &>(logical);
        return std::make_unique<PhysicalInsert>(
            insert.GetTableOid(), insert.GetTargetColumns(), CloneRows(insert.GetRows()));
    }
    case LogicalPlan::Type::CREATE_TABLE: {
        const auto &create = static_cast<const LogicalCreateTable &>(logical);
        return std::make_unique<PhysicalCreateTable>(create.GetTableName(), create.GetColumns());
    }
    }
    throw std::runtime_error("physical planner: unsupported logical node");
}

} // namespace simple_olap
