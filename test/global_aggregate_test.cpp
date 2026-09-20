// Global Aggregate Fast Path 验证程序（手工编译或通过 CMake test target 构建）。
//
// 编译（在项目根目录）：
//   g++ -std=c++17 -O2 -Isrc test/global_aggregate_test.cpp \
//       build/lib/libsimple_olap_core.a -lpthread -o /tmp/global_aggregate_test
//
// 覆盖：
//   1. 空表 global aggregate：single == multi，且输出 1 行（COUNT/SUM/AVG/MIN/MAX = 0）
//   2. COUNT(*) / COUNT(col) / SUM / AVG / MIN / MAX / 多聚合组合
//   3. WHERE 0% / 100% / ~50% 三种 active row 分布
//   4. expression aggregate（SUM(id + 1)）
//   5. GROUP BY（GROUPED_HASH 路径）无回归
//   6. 所有查询 single 与 multi 结果一致

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "catalog/catalog.h"
#include "execution/expression/exec_expression.h"
#include "main/connection.h"
#include "main/database.h"
#include "main/query_result.h"
#include "storage/datachunk.h"
#include "storage/table/table_storage.h"
#include "type.h"

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

// ---------- 结果读取 ----------

using Row = std::vector<ExecValue>;

static std::vector<Row> CollectRows(const QueryResult& result) {
    std::vector<Row> rows;
    for (const auto& batch : result.chunks) {
        for (uint32_t row = 0; row < batch.size; ++row) {
            Row values;
            values.reserve(batch.columns.size());
            for (const auto& col : batch.columns) {
                values.push_back(ReadExecValue(col, row));
            }
            rows.push_back(std::move(values));
        }
    }
    return rows;
}

// 数值按相对容差比较（多线程 SUM/AVG 的加法结合顺序与单线程不同）。
static bool ValueEqual(const ExecValue& a, const ExecValue& b) {
    if (std::holds_alternative<std::string>(a) || std::holds_alternative<std::string>(b)) {
        return a == b;
    }
    const long double x = ExecValueAsNumber(a);
    const long double y = ExecValueAsNumber(b);
    const long double diff = std::fabs(x - y);
    const long double scale = std::max<long double>(1.0L, std::max(std::fabs(x), std::fabs(y)));
    return diff <= 1e-9L * scale;
}

static void CheckRowsEqual(const std::vector<Row>& a, const std::vector<Row>& b, const std::string& label) {
    if (a.size() != b.size()) {
        Fail(label + ": row count mismatch (" + std::to_string(a.size()) + " vs " + std::to_string(b.size()) + ")");
        return;
    }
    for (size_t r = 0; r < a.size(); ++r) {
        if (a[r].size() != b[r].size()) {
            Fail(label + ": column count mismatch");
            continue;
        }
        for (size_t c = 0; c < a[r].size(); ++c) {
            if (!ValueEqual(a[r][c], b[r][c])) {
                Fail(label + ": value mismatch at row " + std::to_string(r) + " col " + std::to_string(c));
            }
        }
    }
}

// 在 single / multi 两种模式下执行同一 SQL，校验结果一致，返回 single 的结果。
static std::vector<Row> RunBothModes(Database& database, Connection& connection, const std::string& sql,
                                     const std::string& label) {
    database.SetExecutionMode(ExecutionMode::SINGLE_THREAD);
    const std::vector<Row> single = CollectRows(connection.Query(sql));

    database.SetExecutionMode(ExecutionMode::MULTI_THREAD);
    const std::vector<Row> multi = CollectRows(connection.Query(sql));

    CheckRowsEqual(single, multi, label + " [single == multi]");

    database.SetExecutionMode(ExecutionMode::AUTO);
    return single;
}

static void CheckSingleRow(const std::vector<Row>& rows, const std::vector<ExecValue>& expected,
                           const std::string& label) {
    if (rows.size() != 1 || rows[0].size() != expected.size()) {
        Fail(label + ": expected exactly one row with " + std::to_string(expected.size()) + " columns");
        return;
    }
    for (size_t c = 0; c < expected.size(); ++c) {
        if (!ValueEqual(rows[0][c], expected[c])) {
            Fail(label + ": value mismatch at column " + std::to_string(c));
        }
    }
}

// ---------- 期望值（手算参考） ----------

static ExecValue I64(int64_t v) {
    return v;
}

static ExecValue D(double v) {
    return v;
}

int main() {
    const std::filesystem::path db_path = "/tmp/simple_olap_global_aggregate_test";
    std::filesystem::remove_all(db_path);

    Database database(db_path);
    Connection connection(database);

    connection.Query("CREATE TABLE agg_t (id INT, g INT, value DOUBLE);");

    // ---------- 1. 空表：single == multi，且输出一行 0 ----------
    {
        const std::string sql = "SELECT COUNT(*), SUM(id), AVG(value), MIN(id), MAX(value) FROM agg_t;";
        const std::vector<Row> rows = RunBothModes(database, connection, sql, "empty global aggregate");
        CheckSingleRow(rows, {I64(0), I64(0), D(0.0), D(0.0), D(0.0)}, "empty global aggregate values");
    }

    // ---------- 2. 写入数据 ----------
    constexpr uint32_t kN = 3000;
    constexpr int32_t kGroups = 7;

    std::ostringstream insert;
    uint32_t stmt_rows = 0;
    for (uint32_t i = 0; i < kN; ++i) {
        if (stmt_rows == 0) {
            insert << "INSERT INTO agg_t VALUES ";
        } else {
            insert << ", ";
        }
        insert << "(" << i << ", " << (i % kGroups) << ", "
               << std::to_string(static_cast<double>(i) * 0.5 + 1.0) << ")";
        ++stmt_rows;
        if (stmt_rows == 500 || i + 1 == kN) {
            insert << ";";
            connection.Query(insert.str());
            insert.str("");
            insert.clear();
            stmt_rows = 0;
        }
    }

    // INSERT 先进入内存，Scan 只读已落盘 segment：显式 Flush 让数据可见。
    database.GetStorageManager().Flush();

    // 手算期望值
    long double sum_id = 0.0L;
    long double sum_value = 0.0L;
    int32_t min_id = INT32_MAX;
    int32_t max_id = INT32_MIN;
    double min_value = 1e300;
    double max_value = -1e300;
    long double sum_id_lt_half = 0.0L;
    uint32_t count_lt_half = 0;
    std::map<int32_t, std::pair<int64_t, long double>> groups;
    for (uint32_t i = 0; i < kN; ++i) {
        const int32_t id = static_cast<int32_t>(i);
        const double value = static_cast<double>(i) * 0.5 + 1.0;
        sum_id += id;
        sum_value += value;
        min_id = std::min(min_id, id);
        max_id = std::max(max_id, id);
        min_value = std::min(min_value, value);
        max_value = std::max(max_value, value);
        if (id < static_cast<int32_t>(kN / 2)) {
            ++count_lt_half;
            sum_id_lt_half += id;
        }
        auto& g = groups[id % kGroups];
        g.first += 1;
        g.second += id;
    }

    // ---------- 3. COUNT(*) / COUNT(col) ----------
    CheckSingleRow(RunBothModes(database, connection, "SELECT COUNT(*) FROM agg_t;", "count(*)"), {I64(kN)},
                   "count(*)");
    CheckSingleRow(RunBothModes(database, connection, "SELECT COUNT(id) FROM agg_t;", "count(id)"), {I64(kN)},
                   "count(id)");

    // ---------- 4. SUM / AVG ----------
    CheckSingleRow(RunBothModes(database, connection, "SELECT SUM(id) FROM agg_t;", "sum(id)"),
                   {I64(static_cast<int64_t>(sum_id))}, "sum(id)");
    CheckSingleRow(RunBothModes(database, connection, "SELECT SUM(value) FROM agg_t;", "sum(value)"),
                   {D(static_cast<double>(sum_value))}, "sum(value)");
    CheckSingleRow(RunBothModes(database, connection, "SELECT AVG(value) FROM agg_t;", "avg(value)"),
                   {D(static_cast<double>(sum_value / kN))}, "avg(value)");

    // ---------- 5. MIN / MAX 与多聚合组合 ----------
    CheckSingleRow(RunBothModes(database, connection,
                                "SELECT MIN(id), MAX(id), MIN(value), MAX(value) FROM agg_t;", "min/max"),
                   {I64(min_id), I64(max_id), D(min_value), D(max_value)}, "min/max");
    CheckSingleRow(RunBothModes(database, connection,
                                "SELECT COUNT(*), SUM(id), AVG(value), MIN(id), MAX(value) FROM agg_t;", "combined"),
                   {I64(kN), I64(static_cast<int64_t>(sum_id)), D(static_cast<double>(sum_value / kN)), I64(min_id),
                    D(max_value)},
                   "combined");

    // ---------- 6. expression aggregate ----------
    CheckSingleRow(RunBothModes(database, connection, "SELECT SUM(id + 1) FROM agg_t;", "sum(id+1)"),
                   {I64(static_cast<int64_t>(sum_id + kN))}, "sum(id+1)");

    // ---------- 7. WHERE 分布：~50% / 0% / 100% ----------
    CheckSingleRow(RunBothModes(database, connection,
                                "SELECT COUNT(*), SUM(id) FROM agg_t WHERE id < 1500;", "50% filter"),
                   {I64(count_lt_half), I64(static_cast<int64_t>(sum_id_lt_half))}, "50% filter");
    CheckSingleRow(RunBothModes(database, connection,
                                "SELECT COUNT(*), SUM(id), AVG(value), MIN(id), MAX(value) FROM agg_t WHERE id < 0;",
                                "0% filter"),
                   {I64(0), I64(0), D(0.0), D(0.0), D(0.0)}, "0% filter");
    CheckSingleRow(RunBothModes(database, connection,
                                "SELECT COUNT(*), SUM(id), MIN(id), MAX(value) FROM agg_t WHERE id >= 0;",
                                "100% filter"),
                   {I64(kN), I64(static_cast<int64_t>(sum_id)), I64(min_id), D(max_value)}, "100% filter");

    // ---------- 8. GROUP BY 回归 ----------
    {
        const std::string sql = "SELECT g, COUNT(*), SUM(id) FROM agg_t GROUP BY g;";
        database.SetExecutionMode(ExecutionMode::SINGLE_THREAD);
        const std::vector<Row> single = CollectRows(connection.Query(sql));
        database.SetExecutionMode(ExecutionMode::MULTI_THREAD);
        const std::vector<Row> multi = CollectRows(connection.Query(sql));
        database.SetExecutionMode(ExecutionMode::AUTO);

        if (single.size() != groups.size() || multi.size() != groups.size()) {
            Fail("group by: group count mismatch");
        } else {
            auto check = [&](const std::vector<Row>& rows, const std::string& mode) {
                for (const Row& row : rows) {
                    if (row.size() != 3) {
                        Fail("group by " + mode + ": column count");
                        continue;
                    }
                    const int32_t g = static_cast<int32_t>(ExecValueAsNumber(row[0]));
                    auto it = groups.find(g);
                    if (it == groups.end()) {
                        Fail("group by " + mode + ": unexpected group");
                        continue;
                    }
                    if (static_cast<int64_t>(ExecValueAsNumber(row[1])) != it->second.first ||
                        std::fabs(static_cast<double>(ExecValueAsNumber(row[2]) - it->second.second)) > 1e-6) {
                        Fail("group by " + mode + ": value mismatch");
                    }
                }
            };
            check(single, "single");
            check(multi, "multi");
        }
    }

    // GROUP BY 无匹配行：两种模式都必须输出 0 行（不能像 global 一样补一行）。
    {
        const std::string sql = "SELECT g, COUNT(*) FROM agg_t WHERE id < 0 GROUP BY g;";
        database.SetExecutionMode(ExecutionMode::SINGLE_THREAD);
        const std::vector<Row> single = CollectRows(connection.Query(sql));
        database.SetExecutionMode(ExecutionMode::MULTI_THREAD);
        const std::vector<Row> multi = CollectRows(connection.Query(sql));
        database.SetExecutionMode(ExecutionMode::AUTO);
        Check(single.empty(), "empty group by: single outputs no rows");
        Check(multi.empty(), "empty group by: multi outputs no rows");
    }

    // ---------- 9. 多 segment 并行扫描（morsel-driven global/local scan state） ----------
    //
    // 前面的 agg_t 只有 3000 行（1 个 segment），并行路径实际只有 1 个 worker
    // 有效。本节直接通过 storage API 写入 >2 个 segment，覆盖：
    //   - N 个 worker 各自 atomic 领取 segment，不重不漏
    //   - metadata pruning（某 segment 全部命中 / 部分命中 / 全部跳过）
    //   - row filter mask（pruning 无法决定的 segment 逐行过滤）
    //   - worker 数 > / < segment 数的两种截断
    {
        connection.Query("CREATE TABLE multi_t (id INT, g INT, value DOUBLE);");

        const TableCatalogEntry* entry = database.GetCatalog().GetTable("multi_t");
        Check(entry != nullptr, "multi_t catalog entry");
        Check(entry != nullptr && !entry->schema.columns.empty() && entry->schema.columns[0].type == DataType::INT32,
              "multi_t id column is INT32");
        auto storage = database.GetStorageManager().GetTable(entry->table_id, entry->schema);
        Check(storage != nullptr, "multi_t storage");

        constexpr uint32_t kMultiN = 150000; // 3 个 segment（> 2 * kMaxSegmentRowCount）
        constexpr int32_t kMultiGroups = 5;

        uint32_t expected_count = 0;
        long double expected_sum = 0.0L;
        int32_t expected_min = INT32_MAX;
        int32_t expected_max = INT32_MIN;
        uint32_t expected_count_ge = 0;
        long double expected_sum_ge = 0.0L;
        std::map<int32_t, std::pair<int64_t, long double>> expected_groups;

        uint32_t written = 0;
        while (written < kMultiN) {
            const uint32_t n = static_cast<uint32_t>(std::min<uint32_t>(8192, kMultiN - written));

            auto id_buf = std::shared_ptr<uint8_t[]>(new uint8_t[static_cast<size_t>(n) * sizeof(int32_t)]);
            auto g_buf = std::shared_ptr<uint8_t[]>(new uint8_t[static_cast<size_t>(n) * sizeof(int32_t)]);
            auto value_buf = std::shared_ptr<uint8_t[]>(new uint8_t[static_cast<size_t>(n) * sizeof(double)]);
            auto* ids = reinterpret_cast<int32_t*>(id_buf.get());
            auto* gs = reinterpret_cast<int32_t*>(g_buf.get());
            auto* values = reinterpret_cast<double*>(value_buf.get());

            for (uint32_t r = 0; r < n; ++r) {
                const int32_t id = static_cast<int32_t>(written + r);
                const int32_t g = id % kMultiGroups;
                const double value = static_cast<double>(id) * 0.25 + 2.0;

                ids[r] = id;
                gs[r] = g;
                values[r] = value;

                ++expected_count;
                expected_sum += id;
                expected_min = std::min(expected_min, id);
                expected_max = std::max(expected_max, id);
                if (id >= static_cast<int32_t>(kMultiN * 2 / 3)) {
                    ++expected_count_ge;
                    expected_sum_ge += id;
                }
                auto& group = expected_groups[g];
                group.first += 1;
                group.second += id;
            }

            DataChunk chunk(n);
            chunk.set_data(0, id_buf, sizeof(int32_t));
            chunk.set_data(1, g_buf, sizeof(int32_t));
            chunk.set_data(2, value_buf, sizeof(double));
            storage->Append(chunk);

            written += n;
        }

        // Flush 后活跃 segment 封存落盘，segment_ids 才包含全部 3 个 segment
        database.GetStorageManager().Flush();
        Check(storage->segment_count() >= 3, "multi_t spans at least 3 segments");

        // 9.1 全局聚合：N worker partial aggregate + coordinator merge
        CheckSingleRow(RunBothModes(database, connection,
                                    "SELECT COUNT(*), SUM(id), MIN(id), MAX(id) FROM multi_t;", "multi global"),
                       {I64(expected_count), I64(static_cast<int64_t>(expected_sum)), I64(expected_min),
                        I64(expected_max)},
                       "multi global values");

        // 9.2 metadata pruning + row filter：第一段全部跳过、第二段逐行过滤、第三段全部命中
        CheckSingleRow(RunBothModes(database, connection,
                                    "SELECT COUNT(*), SUM(id) FROM multi_t WHERE id >= 100000;", "multi filter"),
                       {I64(expected_count_ge), I64(static_cast<int64_t>(expected_sum_ge))}, "multi filter values");

        // 9.3 0% 命中：全部 segment 被 metadata pruning 跳过，global aggregate 仍输出一行 0
        CheckSingleRow(
            RunBothModes(database, connection, "SELECT COUNT(*), SUM(id) FROM multi_t WHERE id < 0;", "multi 0%"),
            {I64(0), I64(0)}, "multi 0% values");

        // 9.4 GROUP BY 回归：按 group key 校验（并行输出顺序不做保证）
        {
            const std::string sql = "SELECT g, COUNT(*), SUM(id) FROM multi_t GROUP BY g;";
            database.SetExecutionMode(ExecutionMode::SINGLE_THREAD);
            const std::vector<Row> single = CollectRows(connection.Query(sql));
            database.SetExecutionMode(ExecutionMode::MULTI_THREAD);
            const std::vector<Row> multi = CollectRows(connection.Query(sql));
            database.SetExecutionMode(ExecutionMode::AUTO);

            if (single.size() != expected_groups.size() || multi.size() != expected_groups.size()) {
                Fail("multi group by: group count mismatch");
            } else {
                auto check = [&](const std::vector<Row>& rows, const std::string& mode) {
                    for (const Row& row : rows) {
                        const int32_t g = static_cast<int32_t>(ExecValueAsNumber(row[0]));
                        auto it = expected_groups.find(g);
                        if (it == expected_groups.end()) {
                            Fail("multi group by " + mode + ": unexpected group");
                            continue;
                        }
                        if (static_cast<int64_t>(ExecValueAsNumber(row[1])) != it->second.first ||
                            std::fabs(static_cast<double>(ExecValueAsNumber(row[2]) - it->second.second)) > 1e-6) {
                            Fail("multi group by " + mode + ": value mismatch");
                        }
                    }
                };
                check(single, "single");
                check(multi, "multi");
            }
        }

        // 9.5 并发查询同一张表：扫描状态必须 query-local，
        //     两个查询不能争抢同一个 next_segment。
        {
            database.SetExecutionMode(ExecutionMode::MULTI_THREAD);
            Connection connection_a(database);
            Connection connection_b(database);

            const std::string sql = "SELECT COUNT(*), SUM(id) FROM multi_t WHERE id >= 50000;";
            std::vector<Row> rows_a;
            std::vector<Row> rows_b;

            std::thread thread_a([&] { rows_a = CollectRows(connection_a.Query(sql)); });
            std::thread thread_b([&] { rows_b = CollectRows(connection_b.Query(sql)); });
            thread_a.join();
            thread_b.join();

            database.SetExecutionMode(ExecutionMode::AUTO);
            CheckRowsEqual(rows_a, rows_b, "concurrent queries on same table");
        }
    }

    if (g_failures == 0) {
        std::printf("ALL GLOBAL AGGREGATE TESTS PASSED\n");
        return 0;
    }
    std::printf("%d FAILURE(S)\n", g_failures);
    return 1;
}
