#include "physical_planner.h"

#include <algorithm>
#include <optional>
#include <stdexcept>

namespace simple_olap {

namespace {

// 一次成功的键查找：键列的等值取值 + 被消耗的 pushed predicate 下标
struct KeyLookupMatch {
    std::vector<PlanLiteralValue> values;
    std::vector<uint32_t> consumed_predicates;
};

// 字面量能否按列类型编码为索引键；不能时不能用索引，退化 SeqScan
// （如 INT 列 = 1.5：索引键无法表达“无匹配”，而 SeqScan 的行过滤能精确给出 0 行）。
bool LiteralUsableForKey(const TableSchema& schema, ColumnId column_id, const PlanLiteralValue& value) {
    const ColumnSchema* column = FindColumnSchema(schema, column_id);
    if (column == nullptr) {
        return false;
    }

    switch (column->type) {
    case DataType::INT32:
    case DataType::INT64:
        // 整数键只接受整数型字面量；double 字面量交给 SeqScan 精确判断
        return std::holds_alternative<int32_t>(value);
    case DataType::FLOAT:
    case DataType::DOUBLE:
        return std::holds_alternative<int32_t>(value) || std::holds_alternative<double>(value);
    case DataType::VARCHAR:
        return std::holds_alternative<std::string>(value);
    default:
        return false;
    }
}

// 判断 pushed predicates 是否完整覆盖该键的等值条件：
// 每个键列都必须有且只有一个 EQ，任一列缺失、重复或字面量不可编码都放弃索引（保守退化 SeqScan）。
std::optional<KeyLookupMatch> MatchKeyLookup(const TableSchema& schema, const KeySchema& key,
                                             const std::vector<SimplePredicate>& predicates) {
    KeyLookupMatch match;
    match.values.reserve(key.columns.size());
    match.consumed_predicates.reserve(key.columns.size());

    for (ColumnId key_column : key.columns) {
        std::optional<uint32_t> matched;
        for (uint32_t i = 0; i < static_cast<uint32_t>(predicates.size()); ++i) {
            const SimplePredicate& predicate = predicates[i];
            if (predicate.column_index != key_column || predicate.op != CmpOp::EQ) {
                continue;
            }
            if (matched.has_value()) {
                // 同一列出现多个等值条件：无法用一个 lookup 值表达
                return std::nullopt;
            }
            matched = i;
        }
        if (!matched.has_value()) {
            return std::nullopt;
        }
        if (!LiteralUsableForKey(schema, key_column, predicates[*matched].value)) {
            return std::nullopt;
        }

        match.values.push_back(predicates[*matched].value);
        match.consumed_predicates.push_back(*matched);
    }

    return match;
}

// 生成 IndexScan：未被 lookup 消耗的 predicate 作为 residual 交给算子逐行判断
PhysicalPlanPtr BuildIndexScan(uint32_t table_oid, KeyId key_id, KeyLookupMatch match,
                               const std::vector<SimplePredicate>& predicates, const std::vector<uint32_t>& columns) {
    std::vector<SimplePredicate> residual;
    residual.reserve(predicates.size());

    for (uint32_t i = 0; i < static_cast<uint32_t>(predicates.size()); ++i) {
        if (std::find(match.consumed_predicates.begin(), match.consumed_predicates.end(), i) ==
            match.consumed_predicates.end()) {
            residual.push_back(predicates[i]);
        }
    }

    return std::make_unique<PhysicalIndexScan>(table_oid, key_id, std::move(match.values), columns,
                                               std::move(residual));
}

std::vector<NamedPlanExpr> CloneNamed(const std::vector<NamedPlanExpr>& src) {
    std::vector<NamedPlanExpr> out;
    out.reserve(src.size());
    for (const auto& item : src)
        out.emplace_back(item.expr->Clone(), item.alias);
    return out;
}

std::vector<PlanExprPtr> CloneExprs(const std::vector<PlanExprPtr>& src) {
    std::vector<PlanExprPtr> out;
    out.reserve(src.size());
    for (const auto& expr : src)
        out.push_back(expr->Clone());
    return out;
}

std::vector<std::vector<PlanExprPtr>> CloneRows(const std::vector<std::vector<PlanExprPtr>>& src) {
    std::vector<std::vector<PlanExprPtr>> out;
    out.reserve(src.size());
    for (const auto& src_row : src) {
        std::vector<PlanExprPtr> row;
        row.reserve(src_row.size());
        for (const auto& expr : src_row)
            row.push_back(expr->Clone());
        out.push_back(std::move(row));
    }
    return out;
}

} // namespace

PhysicalPlanPtr PhysicalPlanner::CreatePhysicalPlan(const LogicalPlan& logical) const {
    switch (logical.GetType()) {
    case LogicalPlan::Type::SCAN: {
        const auto& scan = static_cast<const LogicalScan&>(logical);
        return PlanScan(scan);
    }
    case LogicalPlan::Type::FILTER: {
        const auto& filter = static_cast<const LogicalFilter&>(logical);
        return std::make_unique<PhysicalFilter>(filter.GetPredicate().Clone(), CreatePhysicalPlan(filter.GetChild()));
    }
    case LogicalPlan::Type::PROJECT: {
        const auto& project = static_cast<const LogicalProject&>(logical);
        return std::make_unique<PhysicalProject>(CloneNamed(project.GetOutputs()),
                                                 CreatePhysicalPlan(project.GetChild()));
    }
    case LogicalPlan::Type::AGGREGATE: {
        const auto& aggregate = static_cast<const LogicalAggregate&>(logical);
        return std::make_unique<PhysicalHashAggregate>(CloneExprs(aggregate.GetGroupBy()),
                                                       CloneNamed(aggregate.GetOutputs()),
                                                       CreatePhysicalPlan(aggregate.GetChild()));
    }
    case LogicalPlan::Type::INSERT: {
        const auto& insert = static_cast<const LogicalInsert&>(logical);
        return std::make_unique<PhysicalInsert>(insert.GetTableOid(), insert.GetTargetColumns(),
                                                CloneRows(insert.GetRows()));
    }
    case LogicalPlan::Type::CREATE_TABLE: {
        const auto& create = static_cast<const LogicalCreateTable&>(logical);
        return std::make_unique<PhysicalCreateTable>(create.GetTableName(), create.GetSchema());
    }
    }
    throw std::runtime_error("physical planner: unsupported logical node");
}

PhysicalPlanPtr PhysicalPlanner::PlanScan(const LogicalScan& scan) const {
    const uint32_t table_oid = scan.GetTableOid();
    const std::vector<uint32_t>& columns = scan.GetRequiredColumns();
    const std::vector<SimplePredicate>& predicates = scan.GetPushedPredicates();

    // Access Path Selection：完整等值主键优先，其次任一完整等值二级键；
    // 都不命中（或拿不到 schema）时退化 SeqScan。
    const TableCatalogEntry* entry = catalog_->GetTable(table_oid);
    if (entry != nullptr) {
        const TableSchema& schema = entry->schema;

        if (schema.primary_key.has_value()) {
            auto match = MatchKeyLookup(schema, *schema.primary_key, predicates);
            if (match.has_value()) {
                return BuildIndexScan(table_oid, schema.primary_key->key_id, std::move(*match), predicates, columns);
            }
        }

        for (const auto& key : schema.secondary_keys) {
            auto match = MatchKeyLookup(schema, key, predicates);
            if (match.has_value()) {
                return BuildIndexScan(table_oid, key.key_id, std::move(*match), predicates, columns);
            }
        }
    }

    return std::make_unique<PhysicalSeqScan>(table_oid, columns, predicates);
}

} // namespace simple_olap
