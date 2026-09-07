#include "logical_plan.h"

#include <sstream>

namespace simple_olap {

namespace {
std::string Pad(size_t n) {
    return std::string(n, ' ');
}

std::string LiteralToString(const PlanLiteralValue& v) {
    if (std::holds_alternative<int32_t>(v))
        return std::to_string(std::get<int32_t>(v));
    if (std::holds_alternative<double>(v))
        return std::to_string(std::get<double>(v));
    return "'" + std::get<std::string>(v) + "'";
}

const char* CmpName(CmpOp op) {
    switch (op) {
    case CmpOp::EQ:
        return "=";
    case CmpOp::NE:
        return "!=";
    case CmpOp::GT:
        return ">";
    case CmpOp::GE:
        return ">=";
    case CmpOp::LT:
        return "<";
    case CmpOp::LE:
        return "<=";
    }
    return "?";
}
} // namespace

std::string LogicalScan::ToString(size_t indent) const {
    std::ostringstream out;
    out << Pad(indent) << "LogicalScan(table=" << table_oid_;
    if (!required_columns_.empty()) {
        out << ", columns=[";
        for (size_t i = 0; i < required_columns_.size(); ++i) {
            if (i)
                out << ",";
            out << required_columns_[i];
        }
        out << "]";
    }
    if (!pushed_predicates_.empty()) {
        out << ", pushed=[";
        for (size_t i = 0; i < pushed_predicates_.size(); ++i) {
            if (i)
                out << ", ";
            out << "#" << pushed_predicates_[i].column_index << " " << CmpName(pushed_predicates_[i].op) << " "
                << LiteralToString(pushed_predicates_[i].value);
        }
        out << "]";
    }
    out << ")";
    return out.str();
}

std::string LogicalFilter::ToString(size_t indent) const {
    return Pad(indent) + "LogicalFilter(" + predicate_->ToString() + ")\n" + child_->ToString(indent + 2);
}

std::string LogicalProject::ToString(size_t indent) const {
    std::ostringstream out;
    out << Pad(indent) << "LogicalProject(";
    for (size_t i = 0; i < outputs_.size(); ++i) {
        if (i)
            out << ", ";
        out << outputs_[i].expr->ToString();
        if (!outputs_[i].alias.empty())
            out << " AS " << outputs_[i].alias;
    }
    out << ")\n" << child_->ToString(indent + 2);
    return out.str();
}

std::string LogicalAggregate::ToString(size_t indent) const {
    std::ostringstream out;
    out << Pad(indent) << "LogicalAggregate(group_by=[";
    for (size_t i = 0; i < group_by_.size(); ++i) {
        if (i)
            out << ", ";
        out << group_by_[i]->ToString();
    }
    out << "], outputs=[";
    for (size_t i = 0; i < outputs_.size(); ++i) {
        if (i)
            out << ", ";
        out << outputs_[i].expr->ToString();
        if (!outputs_[i].alias.empty())
            out << " AS " << outputs_[i].alias;
    }
    out << "])\n" << child_->ToString(indent + 2);
    return out.str();
}

std::string LogicalInsert::ToString(size_t indent) const {
    std::ostringstream out;
    out << Pad(indent) << "LogicalInsert(table=" << table_oid_ << ", rows=" << rows_.size() << ")";
    return out.str();
}

std::string LogicalCreateTable::ToString(size_t indent) const {
    std::ostringstream out;
    out << Pad(indent) << "LogicalCreateTable(" << table_name_ << ", columns=" << columns_.size() << ")";
    return out.str();
}

} // namespace simple_olap
