#pragma once

#include <cstdint>

#include "execution_context.h"
#include "../planner/physical_plan/physical_plan.h"

namespace simple_olap {

struct CommandResult {
    bool success = false;
    uint64_t affected_rows = 0;
};

// CREATE TABLE / INSERT are one-shot commands, not streaming operators.
// Keep this interface small. See DESIGN.md for the storage/catalog APIs that
// should be added before implementing it without planner/AST leakage.
class CommandExecutor {
public:
    explicit CommandExecutor(ExecutionContext *ctx) : ctx_(ctx) {}

    CommandResult Execute(const PhysicalPlan &plan);

private:
    CommandResult ExecuteInsert(const PhysicalInsert &plan);
    CommandResult ExecuteCreateTable(const PhysicalCreateTable &plan);

    ExecutionContext *ctx_ = nullptr;
};

} // namespace simple_olap
