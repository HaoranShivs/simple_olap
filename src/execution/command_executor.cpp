#include "command_executor.h"

#include <stdexcept>

namespace simple_olap {

CommandResult CommandExecutor::Execute(const PhysicalPlan &plan) {
    switch (plan.GetType()) {
    case PhysicalPlan::Type::INSERT:
        return ExecuteInsert(static_cast<const PhysicalInsert &>(plan));
    case PhysicalPlan::Type::CREATE_TABLE:
        return ExecuteCreateTable(static_cast<const PhysicalCreateTable &>(plan));
    default:
        throw std::runtime_error("CommandExecutor: plan is not a command plan");
    }
}

CommandResult CommandExecutor::ExecuteInsert(const PhysicalInsert &) {
    throw std::runtime_error(
        "CommandExecutor::ExecuteInsert: add the storage-neutral InsertRequest API described in DESIGN.md first");
}

CommandResult CommandExecutor::ExecuteCreateTable(const PhysicalCreateTable &) {
    throw std::runtime_error(
        "CommandExecutor::ExecuteCreateTable: add the storage-neutral CreateTableRequest API described in DESIGN.md first");
}

} // namespace simple_olap
