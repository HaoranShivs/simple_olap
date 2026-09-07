#include "database.h"

#include <memory>

#include "execution/command_executor.h"
#include "planner/binder.h"
#include "planner/physical_plan/physical_planner.h"
#include "sql/lexer/lexer.h"
#include "sql/parser/parser.h"

namespace simple_olap {
Connection::Connection(Database& db) : db_(db), arena_allocator_(4 * 1024 * 1024) { // 初始分配 4MB Arena
}

std::unique_ptr<QueryResult> Connection::Execute(const std::string& sql) {
    try {
        // 1. 内存重置：开启新查询，丢弃上一次查询的所有临时内存 (Arena 的精髓)
        arena_allocator_.Reset();

        // 2. Parse (src/sql)
        auto ast = Parse(sql);
        if (!ast)
            return QueryResult::CreateErrorResult("Syntax Error");

        // 3. Bind & Plan (src/planner)
        // Binder 需要访问全局 Catalog 来校验表名和列名是否存在
        auto logical_plan = BindAndPlan(ast);
        if (!logical_plan)
            return QueryResult::CreateErrorResult("Binding Error");

        // 4. Optimize (src/planner/optimizer)
        auto physical_plan = Optimize(std::move(logical_plan));
        if (!physical_plan)
            return QueryResult::CreateErrorResult("Optimization Error");

        // 5. Execute (src/execution)
        // Executor 需要 StorageManager 来读取 Segment，需要 ThreadPool 来并行扫描
        return ExecutePlan(physical_plan);
    } catch (const std::exception& e) {
        return QueryResult::CreateErrorResult(e.what());
    }
}

std::unique_ptr<AST> Connection::Parse(const std::string& sql) {
    Lexer lexer(sql);
    auto tokens = lexer.Tokenize();
    Parser parser(tokens);
    return parser.Parse();
}

std::unique_ptr<LogicalPlan> Connection::BindAndPlan(const std::unique_ptr<AST>& ast) {
    Binder binder(db_.GetCatalog()); // 注入 Catalog
    return binder.CreatePlan(*ast);
}

std::unique_ptr<PhysicalPlan> Connection::Optimize(std::unique_ptr<LogicalPlan> logical_plan) {
    Optimizer optimizer;
    auto optimized_logical = optimizer.Optimize(std::move(logical_plan));
    return PhysicalPlanner::CreatePlan(*optimized_logical);
}

std::unique_ptr<QueryResult> Connection::ExecutePlan(const std::unique_ptr<PhysicalPlan>& plan) {
    // 构建执行上下文，将全局资源和当前查询的内存池传递给执行器
    ExecutionContext ctx(db_.GetStorageManager(), db_.GetThreadPool(), arena_allocator_);
    Executor executor(ctx);
    return executor.Execute(*plan);
}
} // namespace simple_olap
