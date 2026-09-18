// 键功能验证程序（手工编译，不参与库构建）。
//
// 编译（在项目根目录）：
//   g++ -std=c++17 -O2 -Isrc test/key_feature_test.cpp \
//       build/lib/libsimple_olap_core.a -lpthread -o /tmp/key_feature_test
//   /tmp/key_feature_test
//
// 验证内容：
//   1. PhysicalPlanner Access Path Selection：
//      完整等值主键 / 二级键 -> IndexScan，不完整或非等值 -> SeqScan
//   2. 主键唯一性：批内重复、跨 INSERT 重复、重启（索引重建）后重复
//   3. IndexScan 数据正确性：主键点查、二级键多行、residual 过滤、跨 segment 点查
//   4. 二级键非唯一语义 + VARCHAR 主键编码

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "main/connection.h"
#include "main/database.h"
#include "main/query_result.h"
#include "planner/binder.h"
#include "planner/optimizer/optimizer.h"
#include "planner/physical_plan/physical_planner.h"
#include "planner/planner.h"
#include "sql/lexer/lexer.h"
#include "sql/parser/parser.h"
#include "storage/datachunk.h"
#include "storage/datastructs.h"
#include "storage/storage_manager.h"
#include "storage/table/table_storage.h"

using namespace simple_olap;

static int g_failures = 0;

static void Fail(const std::string& label) {
    std::printf("FAIL: %s\n", label.c_str());
    ++g_failures;
}

static void Check(bool condition, const std::string& label) {
    if (!condition) {
        Fail(label);
    }
}

// ==========================================
// 计划构建（与 Connection::Query 相同的阶段链路，停在物理计划）
// ==========================================
static PhysicalPlanPtr BuildPhysicalPlan(Catalog& catalog, const std::string& sql) {
    Lexer lexer;
    Parser parser;
    StatementPtr statement = parser.ParseStatement(lexer.Lex(sql));

    Binder binder(catalog);
    BoundStatementPtr bound = binder.BindStatement(*statement);

    Planner planner;
    LogicalPlanPtr logical = planner.CreateLogicalPlan(*bound);

    Optimizer optimizer;
    optimizer.Optimize(logical);

    PhysicalPlanner physical_planner(catalog);
    return physical_planner.CreatePhysicalPlan(*logical);
}

// 沿 Filter / Project 单输入链向下找到扫描节点（与并行执行器的定位方式一致）
static const PhysicalPlan& FindScanNode(const PhysicalPlan& plan) {
    switch (plan.GetType()) {
    case PhysicalPlan::Type::FILTER:
        return FindScanNode(static_cast<const PhysicalFilter&>(plan).GetChild());
    case PhysicalPlan::Type::PROJECT:
        return FindScanNode(static_cast<const PhysicalProject&>(plan).GetChild());
    default:
        return plan;
    }
}

static void CheckPhysicalPlanType(Catalog& catalog, const std::string& sql, PhysicalPlan::Type expected,
                                  const std::string& label) {
    PhysicalPlanPtr plan = BuildPhysicalPlan(catalog, sql);
    const PhysicalPlan& scan = FindScanNode(*plan);
    Check(scan.GetType() == expected, label + " (plan=" + plan->ToString() + ")");
}

static void CheckIndexKeyId(Catalog& catalog, const std::string& sql, KeyId expected, const std::string& label) {
    PhysicalPlanPtr plan = BuildPhysicalPlan(catalog, sql);
    const PhysicalPlan& scan = FindScanNode(*plan);
    if (scan.GetType() != PhysicalPlan::Type::INDEX_SCAN) {
        Fail(label + " (not an index scan: " + plan->ToString() + ")");
        return;
    }
    const auto& index_scan = static_cast<const PhysicalIndexScan&>(scan);
    Check(index_scan.GetKeyId() == expected, label);
}

// ==========================================
// 查询辅助
// ==========================================
static uint64_t QueryRowCount(Connection& connection, const std::string& sql) {
    QueryResult result = connection.Query(sql);
    uint64_t rows = 0;
    for (const auto& batch : result.chunks) {
        rows += batch.size;
    }
    return rows;
}

static void CheckRowCount(Connection& connection, const std::string& sql, uint64_t expected, const std::string& label) {
    const uint64_t rows = QueryRowCount(connection, sql);
    Check(rows == expected, label + " (rows=" + std::to_string(rows) + ", expected=" + std::to_string(expected) + ")");
}

static void CheckQueryThrows(Connection& connection, const std::string& sql, const std::string& label) {
    bool threw = false;
    try {
        connection.Query(sql);
    } catch (const std::exception&) {
        threw = true;
    }
    Check(threw, label);
}

// ==========================================
// 用例
// ==========================================
static void TestPlanSelection(Catalog& catalog) {
    // 主键点查 -> IndexScan(key_id=0)
    CheckIndexKeyId(catalog, "SELECT * FROM users WHERE id = 1", 0, "PK equality uses primary key index");

    // 二级键完整等值 -> IndexScan(key_id=1)
    CheckIndexKeyId(catalog, "SELECT * FROM users WHERE age = 20", 1, "secondary key equality uses index");

    // 主键与二级键都命中时主键优先
    CheckIndexKeyId(catalog, "SELECT * FROM users WHERE age = 20 AND id = 1", 0, "primary key wins over secondary key");

    // 非等值 / 部分键 / 无键条件 -> SeqScan
    CheckPhysicalPlanType(catalog, "SELECT * FROM users WHERE id > 1", PhysicalPlan::Type::SEQ_SCAN,
                          "range predicate falls back to seq scan");
    CheckPhysicalPlanType(catalog, "SELECT * FROM orders WHERE user_id = 1", PhysicalPlan::Type::SEQ_SCAN,
                          "incomplete composite key falls back to seq scan");
    CheckPhysicalPlanType(catalog, "SELECT * FROM users WHERE city = 'beijing'", PhysicalPlan::Type::SEQ_SCAN,
                          "varchar predicate (not pushed) uses seq scan");

    // 完整复合主键 -> IndexScan
    CheckIndexKeyId(catalog, "SELECT * FROM orders WHERE user_id = 1 AND order_id = 2", 0,
                    "complete composite primary key uses index");

    // 无键表 -> SeqScan
    CheckPhysicalPlanType(catalog, "SELECT * FROM no_key WHERE a = 1", PhysicalPlan::Type::SEQ_SCAN,
                          "table without keys uses seq scan");

    // 未命名二级键也可用
    CheckPhysicalPlanType(catalog, "SELECT * FROM unnamed_key WHERE b = 1", PhysicalPlan::Type::INDEX_SCAN,
                          "unnamed secondary key is usable");

    // 二级键 + residual 条件仍走索引
    CheckIndexKeyId(catalog, "SELECT * FROM users WHERE age = 20 AND id > 2", 1, "secondary key with residual predicate");

    // 字面量与键列类型：不可编码时退化 SeqScan（不能抛错）
    CheckPhysicalPlanType(catalog, "SELECT * FROM users WHERE id = 1.5", PhysicalPlan::Type::SEQ_SCAN,
                          "non-integral literal for integer key falls back to seq scan");
    CheckPhysicalPlanType(catalog, "SELECT * FROM orders WHERE amount = 5", PhysicalPlan::Type::INDEX_SCAN,
                          "integer literal is usable for double key");
}

static void TestUniquenessAndLookup(Database& database, Connection& connection) {
    // ---- 主键唯一性 ----
    CheckQueryThrows(connection, "INSERT INTO users VALUES (1, 30, 'a'), (1, 31, 'b');",
                     "duplicate primary key inside one batch is rejected");
    connection.Query("INSERT INTO users VALUES (1, 30, 'beijing'), (2, 20, 'shanghai'), "
                     "(3, 20, 'beijing'), (4, 40, 'shenzhen');");
    CheckQueryThrows(connection, "INSERT INTO users VALUES (1, 99, 'x');",
                     "duplicate primary key across inserts is rejected");

    // ---- 二级键非唯一 ----
    connection.Query("INSERT INTO orders VALUES (1, 1, 5.0), (1, 2, 5.0);");
    CheckQueryThrows(connection, "INSERT INTO orders VALUES (1, 1, 9.0);",
                     "composite duplicate primary key is rejected");
    CheckQueryThrows(connection, "INSERT INTO varchar_key VALUES ('dup', 1), ('dup', 2);",
                     "duplicate varchar primary key is rejected");

    // ---- 数据落盘后查询：索引路径 ----
    database.GetStorageManager().Flush();

    CheckRowCount(connection, "SELECT * FROM users WHERE id = 1", 1, "primary key point lookup");
    CheckRowCount(connection, "SELECT * FROM users WHERE id = 99", 0, "primary key miss returns nothing");
    CheckRowCount(connection, "SELECT * FROM users WHERE id = 1.5", 0, "non-integral literal returns nothing");
    CheckRowCount(connection, "SELECT user_id FROM orders WHERE amount = 5", 2, "integer literal on double key");
    CheckRowCount(connection, "SELECT id FROM users WHERE age = 20", 2, "secondary key returns all matches");
    CheckRowCount(connection, "SELECT id FROM users WHERE age = 20 AND id > 2", 1, "secondary key with residual filter");
    CheckRowCount(connection, "SELECT user_id FROM orders WHERE user_id = 1 AND order_id = 2", 1,
                  "composite primary key point lookup");
    CheckRowCount(connection, "SELECT * FROM orders WHERE amount = 5.0", 2, "secondary key on double column");

    // ---- 跨 segment：一次 Append 跨越 segment 边界，索引位置必须落在正确段 ----
    const TableId seg_table_id = database.GetCatalog().FindTable("users").value();
    const TableCatalogEntry* seg_entry = database.GetCatalog().GetTable(seg_table_id);
    auto users_storage = database.GetStorageManager().GetTable(seg_table_id, seg_entry->schema);
    Check(users_storage != nullptr, "users storage opened");

    const uint32_t rows = kMaxSegmentRowCount + 10;
    std::shared_ptr<uint8_t[]> id_buffer(new uint8_t[static_cast<size_t>(rows) * sizeof(int32_t)]);
    std::shared_ptr<uint8_t[]> age_buffer(new uint8_t[static_cast<size_t>(rows) * sizeof(int32_t)]);
    auto* ids = reinterpret_cast<int32_t*>(id_buffer.get());
    auto* ages = reinterpret_cast<int32_t*>(age_buffer.get());
    for (uint32_t i = 0; i < rows; ++i) {
        ids[i] = 1000 + static_cast<int32_t>(i);
        ages[i] = 50;
    }

    DataChunk chunk(rows);
    chunk.set_data(0, id_buffer, sizeof(int32_t));
    chunk.set_data(1, age_buffer, sizeof(int32_t));
    users_storage->Append(chunk);

    CheckQueryThrows(connection, "INSERT INTO users VALUES (1000, 1, 'x');",
                     "duplicate detection works across segment boundary");
    database.GetStorageManager().Flush();

    // 第二 segment 内的一行（65536 + 9 = 65545，id = 1000 + 65545）
    CheckRowCount(connection, "SELECT * FROM users WHERE id = 66545", 1, "point lookup in second segment");
    CheckRowCount(connection, "SELECT id FROM users WHERE age = 50", static_cast<uint64_t>(rows),
                  "secondary key lookup spans both segments");
}

static void TestReopenRebuildIndexes(const std::filesystem::path& db_path) {
    // 重启后索引从已落盘 segment 重建：唯一性仍然生效，点查仍然正确
    Database database(db_path);
    Connection connection(database);

    CheckQueryThrows(connection, "INSERT INTO users VALUES (2, 99, 'x');",
                     "duplicate primary key detected after index rebuild");
    connection.Query("INSERT INTO users VALUES (5, 50, 'x');");
    database.GetStorageManager().Flush();

    CheckRowCount(connection, "SELECT * FROM users WHERE id = 5", 1, "point lookup after reopen");
    CheckRowCount(connection, "SELECT * FROM users WHERE id = 2", 1, "pre-existing row after reopen");
}

static void CreateTables(Connection& connection) {
    connection.Query("CREATE TABLE users (id INT PRIMARY KEY, age INT, city VARCHAR, KEY idx_age (age));");
    connection.Query("CREATE TABLE orders (user_id INT, order_id INT, amount DOUBLE, "
                     "PRIMARY KEY (user_id, order_id), KEY idx_amount (amount));");
    connection.Query("CREATE TABLE no_key (a INT, b INT);");
    connection.Query("CREATE TABLE unnamed_key (a INT, b INT, KEY (b));");
    connection.Query("CREATE TABLE varchar_key (name VARCHAR PRIMARY KEY, v INT);");
}

int main() {
    const std::filesystem::path db_path = "/tmp/simple_olap_key_feature_test";
    std::filesystem::remove_all(db_path);

    {
        Database database(db_path);
        Connection connection(database);

        CreateTables(connection);
        TestPlanSelection(database.GetCatalog());
        TestUniquenessAndLookup(database, connection);
    }

    TestReopenRebuildIndexes(db_path);

    if (g_failures == 0) {
        std::printf("ALL KEY FEATURE TESTS PASSED\n");
        return 0;
    }
    std::printf("%d FAILURE(S)\n", g_failures);
    return 1;
}
