#include "executor_builder.h"

#include <algorithm>
#include <stdexcept>
#include <type_traits>

#include "../storage/catalog.h"
#include "../storage/table/table.h"
#include "aggregate/hash_aggregate.h"
#include "expression/exec_expression.h"
#include "filter/filter.h"
#include "projection/projection.h"
#include "scan/seq_scan.h"

namespace simple_olap {

namespace {

const ColumnSchema* FindColumn(const TableSchema& schema, ColumnId column_id) {
    for (const auto& column : schema.columns) {
        if (column.column_id == column_id) {
            return &column;
        }
    }
    return nullptr;
}

bool PlanExprEquivalent(const PlanExpr& lhs, const PlanExpr& rhs) {
    // mini_olap v1: planner already canonicalizes bound column refs and simple
    // expressions. ToString() is sufficient as a structural key here.
    // If expression IDs are added later, replace this with ExprId equality.
    return lhs.GetReturnType() == rhs.GetReturnType() && lhs.ToString() == rhs.ToString();
}

std::optional<ColumnBinding> DirectBinding(const PlanExpr& expr) {
    if (expr.GetType() != PlanExpr::Type::COLUMN_REF) {
        return std::nullopt;
    }
    const auto& col = static_cast<const PlanColumnRef&>(expr);
    return ColumnBinding{col.GetTableOid(), col.GetColumnIndex()};
}

std::optional<uint32_t> FindGroupIndex(const PlanExpr& expr, const std::vector<PlanExprPtr>& groups) {
    for (uint32_t i = 0; i < static_cast<uint32_t>(groups.size()); ++i) {
        if (PlanExprEquivalent(expr, *groups[i])) {
            return i;
        }
    }
    return std::nullopt;
}

} // namespace

BuiltExecutor ExecutorBuilder::Build(const PhysicalPlan& plan) const {
    if (ctx_ == nullptr || ctx_->catalog == nullptr) {
        throw std::runtime_error("ExecutorBuilder: missing execution context/catalog");
    }
    return BuildNode(plan);
}

BuiltExecutor ExecutorBuilder::BuildNode(const PhysicalPlan& plan) const {
    switch (plan.GetType()) {
    case PhysicalPlan::Type::SEQ_SCAN:
        return BuildSeqScan(static_cast<const PhysicalSeqScan&>(plan));
    case PhysicalPlan::Type::FILTER:
        return BuildFilter(static_cast<const PhysicalFilter&>(plan));
    case PhysicalPlan::Type::PROJECT:
        return BuildProject(static_cast<const PhysicalProject&>(plan));
    case PhysicalPlan::Type::HASH_AGGREGATE:
        return BuildHashAggregate(static_cast<const PhysicalHashAggregate&>(plan));
    case PhysicalPlan::Type::INSERT:
    case PhysicalPlan::Type::CREATE_TABLE:
        throw std::runtime_error("ExecutorBuilder: command plan is not a streaming operator tree");
    }
    throw std::runtime_error("ExecutorBuilder: unsupported physical plan node");
}

BuiltExecutor ExecutorBuilder::BuildSeqScan(const PhysicalSeqScan& plan) const {
    Table* table = ctx_->catalog->GetTable(plan.GetTableOid());
    if (table == nullptr) {
        throw std::runtime_error("ExecutorBuilder: scan table not found");
    }

    const TableSchema& table_schema = table->GetSchema();
    std::vector<ColumnId> scan_columns;
    scan_columns.reserve(plan.GetColumns().size());
    for (uint32_t column : plan.GetColumns()) {
        scan_columns.push_back(static_cast<ColumnId>(column));
    }

    // SELECT COUNT(*) or SELECT 1 FROM t can legitimately require no user
    // columns. The current storage API needs one driver column to advance rows,
    // so v1 scans column 0 as a hidden driver. Later a count-only scan can
    // remove this workaround.
    if (scan_columns.empty()) {
        if (table_schema.columns.empty()) {
            throw std::runtime_error("ExecutorBuilder: table has no columns");
        }
        scan_columns.push_back(table_schema.columns.front().column_id);
    }

    ScanOptions options;
    options.columns = scan_columns;

    // Invariant 1:
    //   A pushed predicate must be executable exactly by Storage.
    //   （Optimizer 只下推数值型 column cmp literal，Storage 行过滤可精确执行。）
    //
    // Invariant 2:
    //   Once a predicate is pushed into Scan, it must be removed from the
    //   residual Filter.（避免重复过滤是 Optimizer 的职责，不是 FilterOperator 的。）
    options.predicates.reserve(plan.GetPredicates().size());
    for (const auto& predicate : plan.GetPredicates()) {
        Condition condition;
        condition.column = static_cast<ColumnId>(predicate.column_index);
        condition.op = predicate.op;
        // std::variant 之间不能直接赋值，用 visit 逐类型取出再包装进目标 variant
        condition.value = std::visit(
            [](const auto& v) -> std::variant<int32_t, int64_t, double, std::string> {
                return v; // 所有备选类型都直接透传
            },
            predicate.value);
        options.predicates.push_back(std::move(condition));
    }

    ExecSchema output_schema;
    output_schema.reserve(scan_columns.size());
    for (ColumnId column_id : scan_columns) {
        const ColumnSchema* column = FindColumn(table_schema, column_id);
        if (column == nullptr) {
            throw std::runtime_error("ExecutorBuilder: scan column not found in table schema");
        }
        ExecSlot slot;
        slot.type = column->type;
        slot.source = ColumnBinding{plan.GetTableOid(), column_id};
        slot.name = column->name;
        output_schema.push_back(std::move(slot));
    }

    return BuiltExecutor{std::make_unique<SeqScanOperator>(plan.GetTableOid(), std::move(options), ctx_),
                         std::move(output_schema)};
}

BuiltExecutor ExecutorBuilder::BuildFilter(const PhysicalFilter& plan) const {
    BuiltExecutor child = BuildNode(plan.GetChild());
    ExecExprPtr predicate = CompileExecExpr(plan.GetPredicate(), child.output_schema);

    BuiltExecutor result;
    result.output_schema = child.output_schema;
    result.root = std::make_unique<FilterOperator>(std::move(child.root), std::move(predicate));
    return result;
}

BuiltExecutor ExecutorBuilder::BuildProject(const PhysicalProject& plan) const {
    BuiltExecutor child = BuildNode(plan.GetChild());

    std::vector<ExecExprPtr> expressions;
    ExecSchema output_schema;
    expressions.reserve(plan.GetOutputs().size());
    output_schema.reserve(plan.GetOutputs().size());

    for (const auto& output : plan.GetOutputs()) {
        expressions.push_back(CompileExecExpr(*output.expr, child.output_schema));

        ExecSlot slot;
        slot.type = output.expr->GetReturnType();
        slot.source = DirectBinding(*output.expr);
        slot.name = output.alias;
        output_schema.push_back(std::move(slot));
    }

    return BuiltExecutor{std::make_unique<ProjectionOperator>(std::move(child.root), std::move(expressions)),
                         std::move(output_schema)};
}

BuiltExecutor ExecutorBuilder::BuildHashAggregate(const PhysicalHashAggregate& plan) const {
    BuiltExecutor child = BuildNode(plan.GetChild());

    std::vector<ExecExprPtr> group_exprs;
    group_exprs.reserve(plan.GetGroupBy().size());
    for (const auto& group : plan.GetGroupBy()) {
        group_exprs.push_back(CompileExecExpr(*group, child.output_schema));
    }

    std::vector<AggCallSpec> agg_calls;
    std::vector<AggregateOutputSpec> outputs;
    ExecSchema output_schema;
    outputs.reserve(plan.GetOutputs().size());
    output_schema.reserve(plan.GetOutputs().size());

    for (const auto& output : plan.GetOutputs()) {
        AggregateOutputSpec output_spec;
        output_spec.type = output.expr->GetReturnType();
        output_spec.name = output.alias;

        ExecSlot schema_slot;
        schema_slot.type = output.expr->GetReturnType();
        schema_slot.name = output.alias;

        if (output.expr->GetType() == PlanExpr::Type::AGG_FUNC) {
            const auto& agg = static_cast<const PlanAggExpr&>(*output.expr);
            AggCallSpec call;
            call.type = agg.GetAggType();
            call.result_type = agg.GetReturnType();
            if (agg.GetArg() != nullptr) {
                call.arg = CompileExecExpr(*agg.GetArg(), child.output_schema);
            }

            output_spec.kind = AggregateOutputSpec::Kind::AGGREGATE;
            output_spec.index = static_cast<uint32_t>(agg_calls.size());
            agg_calls.push_back(std::move(call));
        } else {
            auto group_idx = FindGroupIndex(*output.expr, plan.GetGroupBy());
            if (!group_idx.has_value()) {
                throw std::runtime_error("ExecutorBuilder: non-aggregate output must match a GROUP BY expression");
            }
            output_spec.kind = AggregateOutputSpec::Kind::GROUP_KEY;
            output_spec.index = *group_idx;
            schema_slot.source = DirectBinding(*output.expr);
        }

        outputs.push_back(std::move(output_spec));
        output_schema.push_back(std::move(schema_slot));
    }

    return BuiltExecutor{std::make_unique<HashAggregateOperator>(std::move(child.root), std::move(group_exprs),
                                                                 std::move(agg_calls), std::move(outputs)),
                         std::move(output_schema)};
}

} // namespace simple_olap
