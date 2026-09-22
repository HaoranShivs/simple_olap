// benchmark/qps/bench_duckdb_qps.cpp
//
// Compare.md 协议：DuckDB 侧 QPS / latency benchmark（相同 protocol / 相同数据集）。
//
// 构建（官方 amalgamation，Release -O3 -DNDEBUG，无 -march=native）：
//   g++ -std=c++17 -O3 -DNDEBUG -I<duckdb>/src/amalgamation -c <duckdb>/src/amalgamation/duckdb.cpp
//   g++ -std=c++17 -O3 -DNDEBUG -I<duckdb>/src/amalgamation bench_duckdb_qps.cpp duckdb.o -lpthread -ldl
//
// 用法：
//   bench_duckdb_qps --db FILE.duckdb --generate --rows N
//   bench_duckdb_qps --db FILE.duckdb --engine duckdb --commit TAG --dataset medium \
//                    --threads 4 --warmup 2 --measure 5 --csv out.csv
//   bench_duckdb_qps --db FILE.duckdb --verify --query all

#include <sys/resource.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

#include "duckdb.hpp"

#include "qps_workloads.h"

using namespace duckdb;
using namespace olap_compare;

namespace {

std::string Quote(const std::string& text) {
    return "'" + text + "'";
}

void ExecuteOrThrow(Connection& connection, const std::string& sql) {
    auto result = connection.Query(sql);
    if (result->HasError()) {
        throw std::runtime_error("duckdb query failed: " + result->GetError() + " [" + sql + "]");
    }
}

// v1.5 将 SUM/COUNT 等基础函数拆分到 core_functions 扩展。
// 官方 amalgamation 不含该扩展代码，这里按 DuckDB 官方方式一次性 INSTALL/LOAD；
// 扩展缓存于 ~/.duckdb，加载发生在计时之前。
void LoadCoreFunctions(Connection& connection) {
    auto result = connection.Query("INSTALL core_functions; LOAD core_functions;");
    if (result->HasError()) {
        throw std::runtime_error("duckdb core_functions load failed: " + result->GetError());
    }
}

bool GenerateDataset(const std::string& path, uint64_t rows) {
    if (rows == 0) {
        std::fprintf(stderr, "--generate requires --rows N\n");
        return false;
    }
    DuckDB db(path);
    Connection connection(db);
    LoadCoreFunctions(connection);

    ExecuteOrThrow(connection, "CREATE TABLE bench (id BIGINT, grp INTEGER, grp_hi INTEGER, value DOUBLE);");
    ExecuteOrThrow(connection,
                   "INSERT INTO bench SELECT i, CAST(i % 1024 AS INTEGER), CAST(i % 65536 AS INTEGER), "
                   "(i * 48271 % 1000003) / 1000003.0 FROM range(" +
                       std::to_string(rows) + ") t(i);");
    return true;
}

void ConsumeDuck(MaterializedQueryResult& result, ResultAccumulator& acc) {
    while (true) {
        auto chunk = result.Fetch();
        if (!chunk) {
            break;
        }

        const idx_t column_count = chunk->ColumnCount();
        const idx_t row_count = chunk->size();

        for (idx_t row = 0; row < row_count; ++row) {
            for (idx_t column = 0; column < column_count; ++column) {
                const Value value = chunk->GetValue(column, row);
                if (value.IsNull()) {
                    acc.AddInteger(INT64_MIN);
                    continue;
                }

                const LogicalTypeId type = value.type().id();
                if (type == LogicalTypeId::DOUBLE) {
                    acc.AddDouble(value.GetValue<double>());
                } else if (type == LogicalTypeId::FLOAT) {
                    acc.AddDouble(static_cast<double>(value.GetValue<float>()));
                } else {
                    // COUNT / 分组键 / SUM(BIGINT)->HUGEINT 均可用 BIGINT 精确承载
                    acc.AddInteger(value.DefaultCastAs(LogicalType::BIGINT).GetValue<int64_t>());
                }
            }
            acc.EndRow();
        }
    }
}

struct ContextSwitches {
    uint64_t voluntary = 0;
    uint64_t involuntary = 0;
};

ContextSwitches GetContextSwitches() {
    rusage usage{};
    getrusage(RUSAGE_SELF, &usage);
    return ContextSwitches{static_cast<uint64_t>(usage.ru_nvcsw), static_cast<uint64_t>(usage.ru_nivcsw)};
}

double SecondsSince(Clock::time_point start) {
    return std::chrono::duration<double>(Clock::now() - start).count();
}

} // namespace

int main(int argc, char** argv) {
    try {
        const CliOptions opt = ParseCli(argc, argv);
        if (opt.db_path.empty()) {
            std::fprintf(stderr, "--db PATH is required\n");
            return 2;
        }

        if (opt.generate) {
            if (!GenerateDataset(opt.db_path, opt.rows)) {
                return 1;
            }
            std::printf("generated %llu rows into %s\n", static_cast<unsigned long long>(opt.rows),
                        opt.db_path.c_str());
            return 0;
        }

        DuckDB db(opt.db_path);
        Connection connection(db);

        LoadCoreFunctions(connection);
        ExecuteOrThrow(connection, "SET threads=" + std::to_string(opt.threads));
        ExecuteOrThrow(connection, "SET preserve_insertion_order=false");
        ExecuteOrThrow(connection, "SET memory_limit=" + Quote("16GB"));

        const std::vector<const QuerySpec*> queries = SelectQueries(opt.query);

        if (opt.verify) {
            for (const QuerySpec* query : queries) {
                auto result = connection.Query(query->sql);
                if (result->HasError()) {
                    throw std::runtime_error(result->GetError());
                }
                ResultAccumulator acc;
                ConsumeDuck(*result, acc);
                std::printf("verify,%s,%s,%llu,%.0Lf,%.0Lf,%.12Lg\n", opt.engine.c_str(), query->id,
                            static_cast<unsigned long long>(acc.rows), acc.integer_sum, acc.integer_sq_sum,
                            acc.double_sum);
            }
            return 0;
        }

        uint64_t sink = 0;

        for (const QuerySpec* query : queries) {
            // ---- warmup（不计时） ----
            {
                const auto warmup_start = Clock::now();
                do {
                    auto result = connection.Query(query->sql);
                    if (result->HasError()) {
                        throw std::runtime_error(result->GetError());
                    }
                    ResultAccumulator warmup_acc;
                    ConsumeDuck(*result, warmup_acc);
                    sink ^= warmup_acc.Checksum();
                } while (SecondsSince(warmup_start) < opt.warmup_seconds);
            }

            // ---- measurement ----
            ResultAccumulator acc;
            std::vector<uint64_t> latencies_ns;
            latencies_ns.reserve(4096);

            const ContextSwitches cs_before = GetContextSwitches();
            const auto measure_start = Clock::now();
            do {
                const uint64_t t0 = NowNs();
                auto result = connection.Query(query->sql);
                if (result->HasError()) {
                    throw std::runtime_error(result->GetError());
                }
                ConsumeDuck(*result, acc);
                const uint64_t t1 = NowNs();
                latencies_ns.push_back(t1 - t0);
            } while (SecondsSince(measure_start) < opt.measure_seconds);
            const double total_seconds = SecondsSince(measure_start);
            const ContextSwitches cs_after = GetContextSwitches();

            const LatencyStats stats = ComputeStats(std::move(latencies_ns), total_seconds);
            sink ^= acc.Checksum();

            std::printf("engine=%-13s query=%-3s threads=%zu iters=%-6llu qps=%10.2f p50=%9.1fus p95=%9.1fus "
                        "p99=%9.1fus rows=%llu\n",
                        opt.engine.c_str(), query->id, opt.threads,
                        static_cast<unsigned long long>(stats.iterations), stats.qps, stats.p50_us, stats.p95_us,
                        stats.p99_us, static_cast<unsigned long long>(acc.rows));
            std::fflush(stdout);

            AppendCsv(opt.csv_path,
                      FormatBenchmarkRow(opt, opt.rows, *query, stats, acc.rows, acc.Checksum(),
                                         cs_after.voluntary - cs_before.voluntary,
                                         cs_after.involuntary - cs_before.involuntary));
        }

        std::printf("sink=%llu\n", static_cast<unsigned long long>(sink));
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "ERROR: %s\n", error.what());
        return 1;
    }
}
