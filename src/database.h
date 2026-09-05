#pragma once
#include <memory>
#include <string>

namespace simple_olap
{
    class Database
    {
    public:
        explicit Database(const std::string &db_path);
        ~Database() = default;

        // 核心方法：创建一个与数据库交互的连接 (Session)
        std::unique_ptr<Connection> GetConnection();

        // 全局组件访问器
        Catalog &GetCatalog() const { return *catalog_; }
        StorageManager &GetStorageManager() const { return *storage_manager_; }
        ThreadPool &GetThreadPool() const { return *thread_pool_; }

        const std::string &GetDatabasePath() const { return db_path_; }

    private:
        std::string db_path_;

        // 全局共享组件
        std::unique_ptr<Catalog> catalog_;
        std::unique_ptr<StorageManager> storage_manager_;
        std::unique_ptr<ThreadPool> thread_pool_;
    };

    class Connection
    {
    public:
        explicit Connection(Database &db);

        // 核心入口：执行 SQL 并返回流式结果集
        std::unique_ptr<QueryResult> Execute(const std::string &sql);

    private:
        Database &db_;

        // 会话级内存池：每次 Execute 都会 Reset，实现极速内存回收
        ArenaAllocator arena_allocator_;

        // 内部流水线步骤 (对应你的 src/ 目录)
        std::unique_ptr<AST> Parse(const std::string &sql);
        std::unique_ptr<LogicalPlan> BindAndPlan(const std::unique_ptr<AST> &ast);
        std::unique_ptr<PhysicalPlan> Optimize(std::unique_ptr<LogicalPlan> logical_plan);
        std::unique_ptr<QueryResult> ExecutePlan(const std::unique_ptr<PhysicalPlan> &plan);
    };
} // namespace simple_olap
