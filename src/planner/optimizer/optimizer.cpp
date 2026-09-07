#include "optimizer.h"

#include <algorithm>
#include <stdexcept>

namespace simple_olap {

namespace {

// bool ExtractSimplePredicate(const PlanExpr& expr, SimplePredicate& out) {
//     if (expr.GetType() != PlanExpr::Type::BINARY_OP)
//         return false;
//     const auto& binary = static_cast<const PlanBinaryExpr&>(expr);

//     if (binary.GetLeft().GetType() != PlanExpr::Type::COLUMN_REF ||
//         binary.GetRight().GetType() != PlanExpr::Type::LITERAL) {
//         return false;
//     }

//     if (binary.GetOp() != PlanBinaryOp::EQ && binary.GetOp() != PlanBinaryOp::GT &&
//         binary.GetOp() != PlanBinaryOp::LT) {
//         return false;
//     }

//     const auto& column = static_cast<const PlanColumnRef&>(binary.GetLeft());
//     const auto& literal = static_cast<const PlanLiteral&>(binary.GetRight());
//     out.column_index = column.GetColumnIndex();
//     out.op = ToCmpOp(binary.GetOp());
//     out.value = literal.GetValue();
//     return true;
// }
bool IsNumericType(DataType type) {
    return type == DataType::INT32 || type == DataType::INT64 || type == DataType::FLOAT || type == DataType::DOUBLE;
}

bool IsNumericLiteral(const PlanLiteralValue& value) {
    return std::holds_alternative<int32_t>(value) || std::holds_alternative<double>(value);
}

bool ExtractSimplePredicate(const PlanExpr& expr, SimplePredicate& out) {
    if (expr.GetType() != PlanExpr::Type::BINARY_OP) {
        return false;
    }

    const auto& binary = static_cast<const PlanBinaryExpr&>(expr);

    if (binary.GetLeft().GetType() != PlanExpr::Type::COLUMN_REF ||
        binary.GetRight().GetType() != PlanExpr::Type::LITERAL) {
        return false;
    }

    switch (binary.GetOp()) {
    case PlanBinaryOp::EQ:
    case PlanBinaryOp::GT:
    case PlanBinaryOp::LT:
        break;

    default:
        return false;
    }

    const auto& column = static_cast<const PlanColumnRef&>(binary.GetLeft());

    const auto& literal = static_cast<const PlanLiteral&>(binary.GetRight());

    // 非数值条件暂时绝对不能下推。
    //
    // 因为当前 Storage row filter
    // 只能准确处理数值。
    if (!IsNumericType(column.GetReturnType())) {
        return false;
    }

    if (!IsNumericLiteral(literal.GetValue())) {
        return false;
    }

    out.column_index = column.GetColumnIndex();

    out.op = ToCmpOp(binary.GetOp());

    out.value = literal.GetValue();

    return true;
}

void FlattenConjuncts(const PlanExpr& expr, std::vector<PlanExprPtr>& out) {
    if (expr.GetType() == PlanExpr::Type::BINARY_OP) {
        const auto& binary = static_cast<const PlanBinaryExpr&>(expr);

        if (binary.GetOp() == PlanBinaryOp::AND) {
            FlattenConjuncts(binary.GetLeft(), out);

            FlattenConjuncts(binary.GetRight(), out);

            return;
        }
    }

    out.push_back(expr.Clone());
}

PlanExprPtr BuildConjunction(std::vector<PlanExprPtr> exprs) {
    if (exprs.empty()) {
        return nullptr;
    }

    PlanExprPtr result = std::move(exprs[0]);

    for (size_t i = 1; i < exprs.size(); ++i) {
        result = std::make_unique<PlanBinaryExpr>(PlanBinaryOp::AND, std::move(result), std::move(exprs[i]),
                                                  DataType::INT32);
    }

    return result;
}

// bool PushPredicate(LogicalPlanPtr& node) {
//     if (!node)
//         return false;

//     switch (node->GetType()) {
//     case LogicalPlan::Type::FILTER: {
//         auto* filter = static_cast<LogicalFilter*>(node.get());
//         bool changed = PushPredicate(filter->MutableChild());

//         if (filter->MutableChild()->GetType() != LogicalPlan::Type::SCAN)
//             return changed;

//         SimplePredicate predicate;
//         if (!ExtractSimplePredicate(filter->GetPredicate(), predicate))
//             return changed;

//         auto* scan = static_cast<LogicalScan*>(filter->MutableChild().get());
//         if (scan->GetPushedPredicate().has_value())
//             return changed;

//         scan->SetPushedPredicate(std::move(predicate));
//         node = std::move(filter->MutableChild());
//         return true;
//     }
//     case LogicalPlan::Type::PROJECT:
//         return PushPredicate(static_cast<LogicalProject*>(node.get())->MutableChild());
//     case LogicalPlan::Type::AGGREGATE:
//         return PushPredicate(static_cast<LogicalAggregate*>(node.get())->MutableChild());
//     default:
//         return false;
//     }
// }

bool PushPredicate(LogicalPlanPtr& node) {
    if (!node) {
        return false;
    }

    switch (node->GetType()) {
    case LogicalPlan::Type::FILTER: {
        auto* filter = static_cast<LogicalFilter*>(node.get());

        bool changed = PushPredicate(filter->MutableChild());

        // v1 只下推到直接 Scan。
        if (filter->MutableChild()->GetType() != LogicalPlan::Type::SCAN) {
            return changed;
        }

        auto* scan = static_cast<LogicalScan*>(filter->MutableChild().get());

        // 1. WHERE 按 AND 拆开。
        std::vector<PlanExprPtr> conjuncts;

        FlattenConjuncts(filter->GetPredicate(), conjuncts);

        // 2. 分成 pushed / residual。
        std::vector<SimplePredicate> pushed;

        std::vector<PlanExprPtr> residual;

        for (auto& expr : conjuncts) {
            SimplePredicate predicate;

            if (ExtractSimplePredicate(*expr, predicate)) {
                pushed.push_back(std::move(predicate));
            } else {
                residual.push_back(std::move(expr));
            }
        }

        // 一个都推不了。
        if (pushed.empty()) {
            return changed;
        }

        // 3. pushed 从 Filter 中真正移走。
        for (auto& predicate : pushed) {
            scan->AddPushedPredicate(std::move(predicate));
        }

        // 4. 没 residual:
        //
        // Filter 完全消失。
        if (residual.empty()) {
            node = std::move(filter->MutableChild());

            return true;
        }

        // 5. 有 residual:
        //
        // Filter 只保存没下推的部分。
        filter->SetPredicate(BuildConjunction(std::move(residual)));

        return true;
    }

    case LogicalPlan::Type::PROJECT:
        return PushPredicate(static_cast<LogicalProject*>(node.get())->MutableChild());

    case LogicalPlan::Type::AGGREGATE:
        return PushPredicate(static_cast<LogicalAggregate*>(node.get())->MutableChild());

    default:
        return false;
    }
}

void CollectExprColumns(const PlanExpr& expr, std::vector<uint32_t>& columns) {
    expr.CollectColumnRefs(columns);
}

void CollectRequiredColumns(const LogicalPlan& node, std::vector<uint32_t>& columns) {
    switch (node.GetType()) {
    case LogicalPlan::Type::FILTER: {
        const auto& filter = static_cast<const LogicalFilter&>(node);
        CollectExprColumns(filter.GetPredicate(), columns);
        CollectRequiredColumns(filter.GetChild(), columns);
        break;
    }
    case LogicalPlan::Type::PROJECT: {
        const auto& project = static_cast<const LogicalProject&>(node);
        for (const auto& output : project.GetOutputs())
            CollectExprColumns(*output.expr, columns);
        CollectRequiredColumns(project.GetChild(), columns);
        break;
    }
    case LogicalPlan::Type::AGGREGATE: {
        const auto& aggregate = static_cast<const LogicalAggregate&>(node);
        for (const auto& expr : aggregate.GetGroupBy())
            CollectExprColumns(*expr, columns);
        for (const auto& output : aggregate.GetOutputs())
            CollectExprColumns(*output.expr, columns);
        CollectRequiredColumns(aggregate.GetChild(), columns);
        break;
    }
    case LogicalPlan::Type::SCAN: {
        const auto& scan = static_cast<const LogicalScan&>(node);
        // if (scan.GetPushedPredicate())
        //     columns.push_back(scan.GetPushedPredicate()->column_index);
        for (const auto& predicate : scan.GetPushedPredicates()) {
            columns.push_back(predicate.column_index);
        }
        break;
    }
    default:
        break;
    }
}

LogicalScan* FindScan(LogicalPlanPtr& node) {
    if (!node)
        return nullptr;
    switch (node->GetType()) {
    case LogicalPlan::Type::SCAN:
        return static_cast<LogicalScan*>(node.get());
    case LogicalPlan::Type::FILTER:
        return FindScan(static_cast<LogicalFilter*>(node.get())->MutableChild());
    case LogicalPlan::Type::PROJECT:
        return FindScan(static_cast<LogicalProject*>(node.get())->MutableChild());
    case LogicalPlan::Type::AGGREGATE:
        return FindScan(static_cast<LogicalAggregate*>(node.get())->MutableChild());
    default:
        return nullptr;
    }
}

} // namespace

bool PredicatePushdownRule::Apply(LogicalPlanPtr& root) const {
    return PushPredicate(root);
}

bool ColumnPruningRule::Apply(LogicalPlanPtr& root) const {
    std::vector<uint32_t> columns;
    CollectRequiredColumns(*root, columns);
    std::sort(columns.begin(), columns.end());
    columns.erase(std::unique(columns.begin(), columns.end()), columns.end());

    LogicalScan* scan = FindScan(root);
    if (!scan)
        return false;
    if (scan->GetRequiredColumns() == columns)
        return false;
    scan->SetRequiredColumns(std::move(columns));
    return true;
}

Optimizer::Optimizer() {
    rules_.push_back(std::make_unique<PredicatePushdownRule>());
    rules_.push_back(std::make_unique<ColumnPruningRule>());
}

void Optimizer::Optimize(LogicalPlanPtr& root) const {
    // 规则数量很少，固定点迭代足够；上限防止未来错误规则造成死循环。
    for (int pass = 0; pass < 8; ++pass) {
        bool changed = false;
        for (const auto& rule : rules_)
            changed |= rule->Apply(root);
        if (!changed)
            break;
    }
}

} // namespace simple_olap
