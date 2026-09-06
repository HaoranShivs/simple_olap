#pragma once

#include <memory>

#include "execution_context.h"
#include "operator.h"
#include "schema.h"
#include "../planner/physical_plan/physical_plan.h"

namespace simple_olap {

struct BuiltExecutor {
    std::unique_ptr<Operator> root;
    ExecSchema output_schema;
};

class ExecutorBuilder {
public:
    explicit ExecutorBuilder(ExecutionContext *ctx) : ctx_(ctx) {}

    // Query plans only: SEQ_SCAN / FILTER / PROJECT / HASH_AGGREGATE.
    // INSERT / CREATE_TABLE are command plans and should be handled by a small
    // command executor rather than fake streaming operators.
    BuiltExecutor Build(const PhysicalPlan &plan) const;

private:
    BuiltExecutor BuildNode(const PhysicalPlan &plan) const;
    BuiltExecutor BuildSeqScan(const PhysicalSeqScan &plan) const;
    BuiltExecutor BuildFilter(const PhysicalFilter &plan) const;
    BuiltExecutor BuildProject(const PhysicalProject &plan) const;
    BuiltExecutor BuildHashAggregate(const PhysicalHashAggregate &plan) const;

    ExecutionContext *ctx_ = nullptr;
};

} // namespace simple_olap
