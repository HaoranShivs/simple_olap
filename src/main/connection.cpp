#include "connection.h"

#include <algorithm>
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

namespace {

// SQL 前端 + planning 的产物。
struct PlannedQuery {
    StatementPtr statement;
    BoundStatementPtr bound;
    PhysicalPlanPtr physical;
};

PlannedQuery PlanQuery(Database& database, std::string_view sql) {
    // ---------- 1. 词法分析 ----------
    Lexer lexer;
    std::vector<Token> tokens = lexer.Lex(sql);

    // ---------- 2. 语法分析 ----------
    Parser parser;
    StatementPtr statement = parser.ParseStatement(std::move(tokens));

    // ---------- 3. 语义绑定 ----------
    Binder binder(database.GetCatalog());
    BoundStatementPtr bound = binder.BindStatement(*statement);

    // ---------- 4. 逻辑计划 ----------
    Planner planner;
    LogicalPlanPtr logical = planner.CreateLogicalPlan(*bound);

    // ---------- 5. 逻辑优化 ----------
    Optimizer optimizer;
    optimizer.Optimize(logical);

    // ---------- 6. 物理计划 ----------
    PhysicalPlanner physical_planner(database.GetCatalog());
    PhysicalPlanPtr physical = physical_planner.CreatePhysicalPlan(*logical);

    return PlannedQuery{std::move(statement), std::move(bound), std::move(physical)};
}

// 执行一个已规划查询：
//   一条 SQL 一个 QueryMemoryContext（coordinator Arena + worker Arenas），
//   由 RAII 在返回时归还 BlockPool。
ExecutionResult RunPlannedQuery(Database& database, const PhysicalPlan& physical, const BatchConsumer& consumer) {
    // worker Arena 数量与并行执行器的实际 worker 上界一致：
    // min(配置上限, 线程池大小)，且至少 1（供空表 global aggregate 使用）。
    const size_t worker_count = std::max<size_t>(
        1, std::min(database.GetConfig().parallel_config.worker_threads, database.GetThreadPool().thread_count()));

    QueryMemoryContext query_memory(database.GetBlockPool(), worker_count,
                                    database.GetConfig().query_memory_mode);

    ExecutionContext ctx(database.GetCatalog(), database.GetStorageManager(), database.GetThreadPool(), query_memory,
                         database.GetBufferPool());

    // 执行模式来自 DatabaseConfig：可在运行期切换单线程 / 多线程
    ctx.execution_mode = database.GetConfig().execution_mode;

    // 并行参数来自 DatabaseConfig：注入 pipeline worker 数与结果队列容量
    ctx.parallel_config = database.GetConfig().parallel_config;

    ExecutionEngine engine(&ctx);
    return engine.Execute(physical, consumer);
}

} // namespace

// PreparedQuery::Impl 的定义放在 .cpp：header 只需前向声明，保持 include 轻量。
struct PreparedQuery::Impl {
    StatementPtr statement;
    BoundStatementPtr bound;
    PhysicalPlanPtr physical;
};

PreparedQuery::PreparedQuery(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}
PreparedQuery::PreparedQuery() noexcept = default;
PreparedQuery::~PreparedQuery() = default;
PreparedQuery::PreparedQuery(PreparedQuery&& other) noexcept = default;
PreparedQuery& PreparedQuery::operator=(PreparedQuery&& other) noexcept = default;

// 完整物化路径：执行并把每个 batch 深拷贝进 QueryResult。
QueryResult Connection::Query(std::string_view sql) {
    PlannedQuery planned = PlanQuery(database_, sql);

    // 结果元数据（type / columns）在执行前从 (parsed, bound) AST 构建
    QueryResult result;
    result.BuildResultMetadata(*planned.statement, *planned.bound);

    // 执行：每个产出的 batch 通过 Append 深拷贝进结果
    // （结果 chunk 的 owned buffer 从 Database 的 BufferPool 分配）
    const ExecutionResult execution_result =
        RunPlannedQuery(database_, *planned.physical, [&](const VectorBatch& batch, const ExecSchema& /*schema*/) {
            result.Append(batch, &database_.GetBufferPool());
        });

    // 命令计划（INSERT / CREATE_TABLE）：受影响行数由执行结果给出
    if (execution_result.type == ExecutionResultType::COMMAND) {
        result.affected_rows = execution_result.affected_rows;
    }

    return result;
}

// 流式执行路径：不物化结果集。
ExecutionSummary Connection::Execute(std::string_view sql, const BatchConsumer& consumer) {
    PlannedQuery planned = PlanQuery(database_, sql);
    const ExecutionResult execution_result = RunPlannedQuery(database_, *planned.physical, consumer);

    ExecutionSummary summary;
    summary.row_count = execution_result.row_count;
    summary.affected_rows = execution_result.affected_rows;
    return summary;
}

// 预编译：SQL 前端 + planning 只做一次。
PreparedQuery Connection::Prepare(std::string_view sql) {
    PlannedQuery planned = PlanQuery(database_, sql);

    auto impl = std::make_unique<PreparedQuery::Impl>();
    impl->statement = std::move(planned.statement);
    impl->bound = std::move(planned.bound);
    impl->physical = std::move(planned.physical);

    return PreparedQuery(std::move(impl));
}

// 执行已预编译查询：只包含执行阶段。
ExecutionSummary Connection::Execute(const PreparedQuery& query, const BatchConsumer& consumer) {
    if (!query.impl_ || !query.impl_->physical) {
        throw std::runtime_error("Connection::Execute: invalid PreparedQuery");
    }

    const ExecutionResult execution_result = RunPlannedQuery(database_, *query.impl_->physical, consumer);

    ExecutionSummary summary;
    summary.row_count = execution_result.row_count;
    summary.affected_rows = execution_result.affected_rows;
    return summary;
}

} // namespace simple_olap
