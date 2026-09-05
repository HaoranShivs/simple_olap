#include "planner.h"

#include <stdexcept>

namespace simple_olap {

namespace {

std::vector<NamedPlanExpr> BuildOutputs(const std::vector<BoundSelectItem> &items) {
    std::vector<NamedPlanExpr> outputs;
    outputs.reserve(items.size());
    for (const auto &item : items) {
        outputs.emplace_back(BuildPlanExpr(*item.expr), item.alias);
    }
    return outputs;
}

bool HasAggregate(const std::vector<NamedPlanExpr> &outputs) {
    for (const auto &output : outputs) {
        if (output.expr->ContainsAggregate()) return true;
    }
    return false;
}

} // namespace

LogicalPlanPtr Planner::CreateLogicalPlan(const BoundStatement &statement) const {
    switch (statement.GetType()) {
    case BoundStatement::Type::SELECT:
        return PlanSelect(static_cast<const BoundSelectStatement &>(statement));
    case BoundStatement::Type::INSERT:
        return PlanInsert(static_cast<const BoundInsertStatement &>(statement));
    case BoundStatement::Type::CREATE_TABLE:
        return PlanCreateTable(static_cast<const BoundCreateTableStatement &>(statement));
    case BoundStatement::Type::EXPLAIN:
        throw std::runtime_error("planner: EXPLAIN should wrap a planned statement; BoundExplainStatement is not defined yet");
    }
    throw std::runtime_error("planner: unsupported statement type");
}

LogicalPlanPtr Planner::PlanSelect(const BoundSelectStatement &statement) const {
    LogicalPlanPtr root = std::make_unique<LogicalScan>(statement.table_oid);

    if (statement.where_clause) {
        root = std::make_unique<LogicalFilter>(BuildPlanExpr(*statement.where_clause), std::move(root));
    }

    auto outputs = BuildOutputs(statement.select_list);
    const bool aggregate_query = !statement.group_by.empty() || HasAggregate(outputs);

    if (aggregate_query) {
        std::vector<PlanExprPtr> group_by;
        group_by.reserve(statement.group_by.size());
        for (const auto &expr : statement.group_by) {
            group_by.push_back(BuildPlanExpr(*expr));
        }
        return std::make_unique<LogicalAggregate>(
            std::move(group_by), std::move(outputs), std::move(root));
    }

    return std::make_unique<LogicalProject>(std::move(outputs), std::move(root));
}

LogicalPlanPtr Planner::PlanInsert(const BoundInsertStatement &statement) const {
    std::vector<std::vector<PlanExprPtr>> rows;
    rows.reserve(statement.values.size());
    for (const auto &bound_row : statement.values) {
        std::vector<PlanExprPtr> row;
        row.reserve(bound_row.size());
        for (const auto &value : bound_row) {
            row.push_back(BuildPlanExpr(*value));
        }
        rows.push_back(std::move(row));
    }

    return std::make_unique<LogicalInsert>(
        statement.table_oid, statement.target_col_indices, std::move(rows));
}

LogicalPlanPtr Planner::PlanCreateTable(const BoundCreateTableStatement &statement) const {
    return std::make_unique<LogicalCreateTable>(statement.table_name, statement.columns);
}

} // namespace simple_olap
