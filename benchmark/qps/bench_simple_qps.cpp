// benchmark/qps/bench_simple_qps.cpp
//
// Compare.md 协议：simple_olap 侧 QPS / latency benchmark。
// 同一份源码用宏区分两代并行架构：
//   -DSIMPLE_OLAP_QUEUE_ARCH  -> b096352（scan/compute + BoundedBlockingQueue）
//   默认                       -> 8d0906f（morsel-driven pipeline workers）
//
// 用法：
//   bench_simple_qps --db DIR --generate --rows N
//   bench_simple_qps --db DIR --engine simple_direct --commit HASH --dataset medium \
//                    --threads 4 --warmup 2 --measure 5 --csv out.csv
//   bench_simple_qps --db DIR --verify --query all

#include <sys/resource.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "catalog/catalog.h"
#include "execution/batch_utils.h"
#include "execution/expression/exec_expression.h"
#include "main/connection.h"
#include "main/database.h"
#include "main/query_result.h"
#include "storage/datachunk.h"
#include "storage/table/table_storage.h"

#include "qps_workloads.h"

using namespace simple_olap;
using namespace olap_compare;

namespace {

constexpr uint32_t kWriteBatchRows = 8192;

TableSchema BenchSchema() {
    TableSchema schema;
    schema.columns.push_back(ColumnSchema{0, "id", DataType::INT64});
    schema.columns.push_back(ColumnSchema{1, "grp", DataType::INT32});
    schema.columns.push_back(ColumnSchema{2, "grp_hi", DataType::INT32});
    schema.columns.push_back(ColumnSchema{3, "value", DataType::DOUBLE});
    return schema;
}

bool GenerateDataset(Database& database, uint64_t rows) {
    if (rows == 0) {
        std::fprintf(stderr, "--generate requires --rows N\n");
        return false;
    }
    if (!database.CreateTable("bench", BenchSchema())) {
        std::fprintf(stderr, "CreateTable failed\n");
        return false;
    }

    const auto table_id = database.GetCatalog().FindTable("bench");
    if (!table_id.has_value()) {
        return false;
    }
    const TableCatalogEntry* entry = database.GetCatalog().GetTable(*table_id);
    if (entry == nullptr) {
        return false;
    }
    auto storage = database.GetStorageManager().GetTable(*table_id, entry->schema);
    if (storage == nullptr) {
        return false;
    }

    uint64_t written = 0;
    while (written < rows) {
        const uint32_t n = static_cast<uint32_t>(std::min<uint64_t>(kWriteBatchRows, rows - written));

        auto id_buf = std::shared_ptr<uint8_t[]>(new uint8_t[static_cast<size_t>(n) * sizeof(int64_t)]);
        auto grp_buf = std::shared_ptr<uint8_t[]>(new uint8_t[static_cast<size_t>(n) * sizeof(int32_t)]);
        auto hi_buf = std::shared_ptr<uint8_t[]>(new uint8_t[static_cast<size_t>(n) * sizeof(int32_t)]);
        auto value_buf = std::shared_ptr<uint8_t[]>(new uint8_t[static_cast<size_t>(n) * sizeof(double)]);

        auto* ids = reinterpret_cast<int64_t*>(id_buf.get());
        auto* grps = reinterpret_cast<int32_t*>(grp_buf.get());
        auto* his = reinterpret_cast<int32_t*>(hi_buf.get());
        auto* values = reinterpret_cast<double*>(value_buf.get());

        for (uint32_t i = 0; i < n; ++i) {
            const uint64_t row = written + i;
            ids[i] = static_cast<int64_t>(row);
            grps[i] = static_cast<int32_t>(row % 1024);
            his[i] = static_cast<int32_t>(row % 65536);
            values[i] = DeterministicValue(row);
        }

        DataChunk chunk(n);
        chunk.set_data(0, id_buf, sizeof(int64_t));
        chunk.set_data(1, grp_buf, sizeof(int32_t));
        chunk.set_data(2, hi_buf, sizeof(int32_t));
        chunk.set_data(3, value_buf, sizeof(double));
        storage->Append(chunk);

        written += n;
    }

    return storage->Flush();
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

void ConsumeSimple(const QueryResult& result, ResultAccumulator& acc) {
    for (const VectorBatch& batch : result.chunks) {
        ForEachActiveRow(batch, [&](uint32_t row) {
            for (const ColumnData& column : batch.columns) {
                const ExecValue value = ReadExecValue(column, row);
                if (std::holds_alternative<double>(value)) {
                    acc.AddDouble(std::get<double>(value));
                } else if (std::holds_alternative<float>(value)) {
                    acc.AddDouble(static_cast<double>(std::get<float>(value)));
                } else {
                    acc.AddInteger(static_cast<int64_t>(ExecValueAsNumber(value)));
                }
            }
            acc.EndRow();
        });
    }
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
            DatabaseConfig config;
            Database database(opt.db_path, config);
            if (!GenerateDataset(database, opt.rows)) {
                return 1;
            }
            std::printf("generated %llu rows into %s\n", static_cast<unsigned long long>(opt.rows),
                        opt.db_path.c_str());
            return 0;
        }

        DatabaseConfig config;
        config.execution_mode = ExecutionMode::MULTI_THREAD;
#ifdef SIMPLE_OLAP_QUEUE_ARCH
        config.parallel_config.scan_threads = opt.threads;
        config.parallel_config.compute_threads = opt.threads;
        config.parallel_config.batch_queue_capacity = 64;
        config.thread_count = std::max(config.thread_count, opt.threads + 2);
#else
        config.parallel_config.worker_threads = opt.threads;
        config.thread_count = std::max(config.thread_count, opt.threads);
#endif

        Database database(opt.db_path, config);
        Connection connection(database);

        const std::vector<const QuerySpec*> queries = SelectQueries(opt.query);

        if (opt.verify) {
            for (const QuerySpec* query : queries) {
                ResultAccumulator acc;
                ConsumeSimple(connection.Query(query->sql), acc);
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
                    ResultAccumulator warmup_acc;
                    ConsumeSimple(connection.Query(query->sql), warmup_acc);
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
                QueryResult result = connection.Query(query->sql);
                ConsumeSimple(result, acc);
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
