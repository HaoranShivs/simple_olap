#include "plan_expression.h"

#include <sstream>
#include <stdexcept>

namespace simple_olap {

namespace {

const char *BinaryOpName(PlanBinaryOp op) {
    switch (op) {
    case PlanBinaryOp::ADD: return "+";
    case PlanBinaryOp::SUB: return "-";
    case PlanBinaryOp::EQ: return "=";
    case PlanBinaryOp::GT: return ">";
    case PlanBinaryOp::LT: return "<";
    case PlanBinaryOp::AND: return "AND";
    case PlanBinaryOp::OR: return "OR";
    }
    return "?";
}

const char *AggName(AggType type) {
    switch (type) {
    case AggType::SUM: return "SUM";
    case AggType::COUNT: return "COUNT";
    case AggType::AVG: return "AVG";
    case AggType::MIN: return "MIN";
    case AggType::MAX: return "MAX";
    default: return "INVALID";
    }
}

} // namespace

PlanExprPtr PlanColumnRef::Clone() const {
    return std::make_unique<PlanColumnRef>(table_oid_, col_idx_, GetReturnType());
}

std::string PlanColumnRef::ToString() const {
    return "#" + std::to_string(table_oid_) + "." + std::to_string(col_idx_);
}

void PlanColumnRef::CollectColumnRefs(std::vector<uint32_t> &out) const {
    out.push_back(col_idx_);
}

PlanExprPtr PlanLiteral::Clone() const {
    return std::make_unique<PlanLiteral>(value_, GetReturnType());
}

std::string PlanLiteral::ToString() const {
    if (std::holds_alternative<int32_t>(value_)) {
        return std::to_string(std::get<int32_t>(value_));
    }
    if (std::holds_alternative<double>(value_)) {
        return std::to_string(std::get<double>(value_));
    }
    return "'" + std::get<std::string>(value_) + "'";
}

PlanExprPtr PlanBinaryExpr::Clone() const {
    return std::make_unique<PlanBinaryExpr>(op_, left_->Clone(), right_->Clone(), GetReturnType());
}

std::string PlanBinaryExpr::ToString() const {
    return "(" + left_->ToString() + " " + BinaryOpName(op_) + " " + right_->ToString() + ")";
}

void PlanBinaryExpr::CollectColumnRefs(std::vector<uint32_t> &out) const {
    left_->CollectColumnRefs(out);
    right_->CollectColumnRefs(out);
}

bool PlanBinaryExpr::ContainsAggregate() const {
    return left_->ContainsAggregate() || right_->ContainsAggregate();
}

PlanExprPtr PlanAggExpr::Clone() const {
    return std::make_unique<PlanAggExpr>(agg_type_, arg_ ? arg_->Clone() : nullptr, GetReturnType());
}

std::string PlanAggExpr::ToString() const {
    return std::string(AggName(agg_type_)) + "(" + (arg_ ? arg_->ToString() : "*") + ")";
}

void PlanAggExpr::CollectColumnRefs(std::vector<uint32_t> &out) const {
    if (arg_) arg_->CollectColumnRefs(out);
}

PlanBinaryOp ToPlanBinaryOp(BinaryOpExpr::OpType op) {
    switch (op) {
    case BinaryOpExpr::OpType::ADD: return PlanBinaryOp::ADD;
    case BinaryOpExpr::OpType::SUB: return PlanBinaryOp::SUB;
    case BinaryOpExpr::OpType::EQ: return PlanBinaryOp::EQ;
    case BinaryOpExpr::OpType::GT: return PlanBinaryOp::GT;
    case BinaryOpExpr::OpType::LT: return PlanBinaryOp::LT;
    case BinaryOpExpr::OpType::AND: return PlanBinaryOp::AND;
    case BinaryOpExpr::OpType::OR: return PlanBinaryOp::OR;
    }
    throw std::runtime_error("planner: unsupported binary operator");
}

CmpOp ToCmpOp(PlanBinaryOp op) {
    switch (op) {
    case PlanBinaryOp::EQ: return CmpOp::EQ;
    case PlanBinaryOp::GT: return CmpOp::GT;
    case PlanBinaryOp::LT: return CmpOp::LT;
    default:
        throw std::runtime_error("planner: expression is not a storage-pushable comparison");
    }
}

PlanExprPtr BuildPlanExpr(const BoundExpr &expr) {
    switch (expr.type) {
    case BoundExpr::Type::COLUMN_REF: {
        const auto &e = static_cast<const BoundColumnRef &>(expr);
        return std::make_unique<PlanColumnRef>(e.table_oid, e.col_idx, e.return_type);
    }
    case BoundExpr::Type::LITERAL: {
        const auto &e = static_cast<const BoundLiteral &>(expr);
        return std::make_unique<PlanLiteral>(e.value, e.return_type);
    }
    case BoundExpr::Type::BINARY_OP: {
        const auto &e = static_cast<const BoundBinaryOp &>(expr);
        return std::make_unique<PlanBinaryExpr>(
            ToPlanBinaryOp(e.op), BuildPlanExpr(*e.left), BuildPlanExpr(*e.right), e.return_type);
    }
    case BoundExpr::Type::AGG_FUNC: {
        const auto &e = static_cast<const BoundAggFunc &>(expr);
        return std::make_unique<PlanAggExpr>(
            e.agg_type, e.arg ? BuildPlanExpr(*e.arg) : nullptr, e.return_type);
    }
    case BoundExpr::Type::CAST:
        throw std::runtime_error("planner: CAST is not implemented in the mini_olap core subset");
    }
    throw std::runtime_error("planner: unknown bound expression type");
}

} // namespace simple_olap
