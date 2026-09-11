#include "connection.h"

#include <algorithm>
#include <iostream>
#include <stdexcept>

#include "../execution/command_executor.h"
#include "../execution/execution_engine.h"
#include "../execution/executor_builder.h"
#include "../execution/operator.h"
#include "../execution/vector/vector.h"
#include "../memory/query_memory_context/query_memory_context.h"
#include "../planner/binder.h"
#include "../planner/optimizer/optimizer.h"
#include "../planner/physical_plan/physical_planner.h"
#include "../planner/planner.h"
#include "../sql/lexer/lexer.h"
#include "../sql/parser/parser.h"

namespace simple_olap {

// 执行一条 SQL：串联 Lexer -> Parser -> Binder -> Planner -> Optimizer
// -> PhysicalPlanner -> ExecutionEngine，返回结果集。
QueryResult Connection::Query(std::string_view sql) {
    // 一条 SQL 一个 QueryMemoryContext：
    //   coordinator Arena + worker Arenas，Query 返回时由 RAII 归还 BlockPool。
    const size_t worker_count = std::max<size_t>(1, database_.GetConfig().parallel_config.compute_threads);

    QueryMemoryContext query_memory(database_.GetBlockPool(), worker_count);

    // ---------- 1. 词法分析 ----------

    Lexer lexer;
    std::vector<Token> tokens = lexer.Lex(sql);

    // ---------- 2. 语法分析 ----------

    Parser parser;
    StatementPtr statement = parser.ParseStatement(std::move(tokens));

    // ---------- 3. 语义绑定 ----------

    Binder binder(database_.GetCatalog());
    BoundStatementPtr bound = binder.BindStatement(*statement);

    // ---------- 4. 逻辑计划 ----------

    Planner planner;
    LogicalPlanPtr logical = planner.CreateLogicalPlan(*bound);

    // ---------- 5. 逻辑优化 ----------

    Optimizer optimizer;
    optimizer.Optimize(logical);

    // ---------- 6. 物理计划 ----------

    PhysicalPlanner physical_planner;
    PhysicalPlanPtr physical = physical_planner.CreatePhysicalPlan(*logical);

    // ---------- 7. 执行 ----------

    ExecutionContext ctx(database_.GetCatalog(), database_.GetStorageManager(), database_.GetThreadPool(), query_memory,
                         database_.GetBufferPool());

    // 执行模式来自 DatabaseConfig：可在运行期切换单线程 / 多线程
    ctx.execution_mode = database_.GetConfig().execution_mode;

    // 并行参数来自 DatabaseConfig：注入 scan/compute 线程数与批队列容量
    ctx.parallel_config = database_.GetConfig().parallel_config;

    ExecutionEngine engine(&ctx);

    QueryResult result;

    // 结果元数据（type / columns）在执行前从 (parsed, bound) AST 构建
    result.BuildResultMetadata(*statement, *bound);

    // 执行：每个产出的 batch 通过 Append 深拷贝进结果
    // （结果 chunk 的 owned buffer 从 Database 的 BufferPool 分配）
    const ExecutionResult execution_result =
        engine.Execute(*physical, [&](const VectorBatch& batch, const ExecSchema& /*schema*/) {
            result.Append(batch, &database_.GetBufferPool());
        });

    // 命令计划（INSERT / CREATE_TABLE）：受影响行数由执行结果给出
    if (execution_result.type == ExecutionResultType::COMMAND) {
        result.affected_rows = execution_result.affected_rows;
    }

    // ---------- 8. 查询收尾 ----------

    // 不再显式 Reset：Query 返回时
    //   ExecutionEngine -> ExecutionContext -> QueryMemoryContext
    //   -> Arena 析构 -> BlockPool::Release()，由 RAII 完成。

    return result;
}
} // namespace simple_olap
