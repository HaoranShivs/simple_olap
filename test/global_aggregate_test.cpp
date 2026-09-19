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
#include <sstream>
#include <string>
#include <vector>

#include "execution/expression/exec_expression.h"
#include "main/connection.h"
#include "main/database.h"
#include "main/query_result.h"
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

    if (g_failures == 0) {
        std::printf("ALL GLOBAL AGGREGATE TESTS PASSED\n");
        return 0;
    }
    std::printf("%d FAILURE(S)\n", g_failures);
    return 1;
}
