#include "connection.h"

#include <iostream>
#include <stdexcept>

#include "../execution/command_executor.h"
#include "../execution/execution_engine.h"
#include "../execution/executor_builder.h"
#include "../execution/operator.h"
#include "../execution/vector/vector.h"
#include "../planner/binder.h"
#include "../planner/optimizer/optimizer.h"
#include "../planner/physical_plan/physical_planner.h"
#include "../planner/planner.h"
#include "../sql/lexer/lexer.h"
#include "../sql/parser/parser.h"

namespace simple_olap {
QueryResult Connection::Query(std::string_view sql) {
    query_arena_.Reset();

    // -------------------------
    // 1. Lexer
    // -------------------------

    Lexer lexer;
    std::vector<Token> tokens = lexer.Lex(sql);

    // -------------------------
    // 2. Parser
    // -------------------------

    Parser parser;
    StatementPtr statement = parser.ParseStatement(std::move(tokens));

    // -------------------------
    // 3. Binder
    // -------------------------

    Binder binder(database_.GetCatalog());
    BoundStatementPtr bound = binder.BindStatement(*statement);

    // -------------------------
    // 4. Logical Planner
    // -------------------------

    Planner planner;
    LogicalPlanPtr logical = planner.CreateLogicalPlan(*bound);

    // -------------------------
    // 5. Optimizer
    // -------------------------

    Optimizer optimizer;
    optimizer.Optimize(logical);

    // -------------------------
    // 6. Physical Planner
    // -------------------------

    PhysicalPlanner physical_planner;
    PhysicalPlanPtr physical = physical_planner.CreatePhysicalPlan(*logical);

    // -------------------------
    // 7. Execution
    // -------------------------

    ExecutionContext ctx(database_.GetCatalog(), database_.GetStorageManager(), database_.GetThreadPool(),
                         query_arena_);

    ExecutionEngine engine(&ctx);

    QueryResult result;

    // 结果元数据（type / columns）在执行前从 (parsed, bound) AST 构建
    result.BuildResultMetadata(*statement, *bound);

    // 执行：每个产出的 batch 通过 Append 深拷贝进结果
    const ExecutionResult execution_result = engine.Execute(
        *physical, [&](const VectorBatch& batch, const ExecSchema& /*schema*/) { result.Append(batch); });

    // 命令计划（INSERT / CREATE_TABLE）：受影响行数由执行结果给出
    if (execution_result.type == ExecutionResultType::COMMAND) {
        result.affected_rows = execution_result.affected_rows;
    }

    // -------------------------
    // 8. Query end
    // -------------------------

    query_arena_.Reset();

    return result;
}
} // namespace simple_olap
