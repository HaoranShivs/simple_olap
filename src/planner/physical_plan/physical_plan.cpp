#include "physical_plan.h"

#include <sstream>

namespace simple_olap {

namespace {
std::string Pad(size_t n) {
    return std::string(n, ' ');
}

} // namespace

std::string PhysicalSeqScan::ToString(size_t indent) const {
    std::ostringstream out;
    out << Pad(indent) << "PhysicalSeqScan(table=" << table_oid_ << ", columns=[";
    for (size_t i = 0; i < columns_.size(); ++i) {
        if (i)
            out << ",";
        out << columns_[i];
    }
    out << "]";
    if (!predicates_.empty())
        out << ", pushed=" << predicates_.size();
    out << ")";
    return out.str();
}

std::string PhysicalFilter::ToString(size_t indent) const {
    return Pad(indent) + "PhysicalFilter(" + predicate_->ToString() + ")\n" + child_->ToString(indent + 2);
}

std::string PhysicalProject::ToString(size_t indent) const {
    std::ostringstream out;
    out << Pad(indent) << "PhysicalProject(" << outputs_.size() << " exprs)\n";
    out << child_->ToString(indent + 2);
    return out.str();
}

std::string PhysicalHashAggregate::ToString(size_t indent) const {
    std::ostringstream out;
    out << Pad(indent) << "PhysicalHashAggregate(groups=" << group_by_.size() << ", outputs=" << outputs_.size()
        << ")\n";
    out << child_->ToString(indent + 2);
    return out.str();
}

std::string PhysicalInsert::ToString(size_t indent) const {
    return Pad(indent) + "PhysicalInsert(table=" + std::to_string(table_oid_) +
           ", rows=" + std::to_string(rows_.size()) + ")";
}

std::string PhysicalCreateTable::ToString(size_t indent) const {
    return Pad(indent) + "PhysicalCreateTable(" + table_name_ + ")";
}

} // namespace simple_olap
