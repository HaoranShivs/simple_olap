#include "optimizer.h"

#include <algorithm>
#include <stdexcept>

namespace simple_olap {

namespace {

bool ExtractSimplePredicate(const PlanExpr &expr, SimplePredicate &out) {
    if (expr.GetType() != PlanExpr::Type::BINARY_OP) return false;
    const auto &binary = static_cast<const PlanBinaryExpr &>(expr);

    if (binary.GetLeft().GetType() != PlanExpr::Type::COLUMN_REF ||
        binary.GetRight().GetType() != PlanExpr::Type::LITERAL) {
        return false;
    }

    if (binary.GetOp() != PlanBinaryOp::EQ &&
        binary.GetOp() != PlanBinaryOp::GT &&
        binary.GetOp() != PlanBinaryOp::LT) {
        return false;
    }

    const auto &column = static_cast<const PlanColumnRef &>(binary.GetLeft());
    const auto &literal = static_cast<const PlanLiteral &>(binary.GetRight());
    out.column_index = column.GetColumnIndex();
    out.op = ToCmpOp(binary.GetOp());
    out.value = literal.GetValue();
    return true;
}

bool PushPredicate(LogicalPlanPtr &node) {
    if (!node) return false;

    switch (node->GetType()) {
    case LogicalPlan::Type::FILTER: {
        auto *filter = static_cast<LogicalFilter *>(node.get());
        bool changed = PushPredicate(filter->MutableChild());

        if (filter->MutableChild()->GetType() != LogicalPlan::Type::SCAN) return changed;

        SimplePredicate predicate;
        if (!ExtractSimplePredicate(filter->GetPredicate(), predicate)) return changed;

        auto *scan = static_cast<LogicalScan *>(filter->MutableChild().get());
        if (scan->GetPushedPredicate().has_value()) return changed;

        scan->SetPushedPredicate(std::move(predicate));
        node = std::move(filter->MutableChild());
        return true;
    }
    case LogicalPlan::Type::PROJECT:
        return PushPredicate(static_cast<LogicalProject *>(node.get())->MutableChild());
    case LogicalPlan::Type::AGGREGATE:
        return PushPredicate(static_cast<LogicalAggregate *>(node.get())->MutableChild());
    default:
        return false;
    }
}

void CollectExprColumns(const PlanExpr &expr, std::vector<uint32_t> &columns) {
    expr.CollectColumnRefs(columns);
}

void CollectRequiredColumns(const LogicalPlan &node, std::vector<uint32_t> &columns) {
    switch (node.GetType()) {
    case LogicalPlan::Type::FILTER: {
        const auto &filter = static_cast<const LogicalFilter &>(node);
        CollectExprColumns(filter.GetPredicate(), columns);
        CollectRequiredColumns(filter.GetChild(), columns);
        break;
    }
    case LogicalPlan::Type::PROJECT: {
        const auto &project = static_cast<const LogicalProject &>(node);
        for (const auto &output : project.GetOutputs()) CollectExprColumns(*output.expr, columns);
        CollectRequiredColumns(project.GetChild(), columns);
        break;
    }
    case LogicalPlan::Type::AGGREGATE: {
        const auto &aggregate = static_cast<const LogicalAggregate &>(node);
        for (const auto &expr : aggregate.GetGroupBy()) CollectExprColumns(*expr, columns);
        for (const auto &output : aggregate.GetOutputs()) CollectExprColumns(*output.expr, columns);
        CollectRequiredColumns(aggregate.GetChild(), columns);
        break;
    }
    case LogicalPlan::Type::SCAN: {
        const auto &scan = static_cast<const LogicalScan &>(node);
        if (scan.GetPushedPredicate()) columns.push_back(scan.GetPushedPredicate()->column_index);
        break;
    }
    default:
        break;
    }
}

LogicalScan *FindScan(LogicalPlanPtr &node) {
    if (!node) return nullptr;
    switch (node->GetType()) {
    case LogicalPlan::Type::SCAN:
        return static_cast<LogicalScan *>(node.get());
    case LogicalPlan::Type::FILTER:
        return FindScan(static_cast<LogicalFilter *>(node.get())->MutableChild());
    case LogicalPlan::Type::PROJECT:
        return FindScan(static_cast<LogicalProject *>(node.get())->MutableChild());
    case LogicalPlan::Type::AGGREGATE:
        return FindScan(static_cast<LogicalAggregate *>(node.get())->MutableChild());
    default:
        return nullptr;
    }
}

} // namespace

bool PredicatePushdownRule::Apply(LogicalPlanPtr &root) const {
    return PushPredicate(root);
}

bool ColumnPruningRule::Apply(LogicalPlanPtr &root) const {
    std::vector<uint32_t> columns;
    CollectRequiredColumns(*root, columns);
    std::sort(columns.begin(), columns.end());
    columns.erase(std::unique(columns.begin(), columns.end()), columns.end());

    LogicalScan *scan = FindScan(root);
    if (!scan) return false;
    if (scan->GetRequiredColumns() == columns) return false;
    scan->SetRequiredColumns(std::move(columns));
    return true;
}

Optimizer::Optimizer() {
    rules_.push_back(std::make_unique<PredicatePushdownRule>());
    rules_.push_back(std::make_unique<ColumnPruningRule>());
}

void Optimizer::Optimize(LogicalPlanPtr &root) const {
    // 规则数量很少，固定点迭代足够；上限防止未来错误规则造成死循环。
    for (int pass = 0; pass < 8; ++pass) {
        bool changed = false;
        for (const auto &rule : rules_) changed |= rule->Apply(root);
        if (!changed) break;
    }
}

} // namespace simple_olap
