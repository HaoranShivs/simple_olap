// ============================================================
// 端到端查询 benchmark：QPS / steady SQL latency / P50/P95/P99
// ============================================================
//
// 严格区分两种测量口径（见 docs/BenchmarkSettingV2.md）：
//
//   [engine]        Connection::Execute(sql, consumer)
//                   = Lexer -> Parser -> Binder -> Planner -> Optimizer
//                     -> PhysicalPlanner -> ExecutionEngine -> blackhole consumer
//                   不物化结果集。这是「数据库执行能力」的 QPS。
//
//   [prepared]      PreparedQuery = Connection::Prepare(sql) 只做一次；
//                   计时只包住 Connection::Execute(prepared, consumer)。
//                   两者差值 = SQL frontend + planning 成本。
//
//   [materialized]  Connection::Query(sql)
//                   = engine + QueryResult 全量深拷贝。
//                   这是当前完整 Query API 的 QPS。
//
// 生命周期：Database 只创建一次；每个 client 一个 Connection；start barrier
// 后统一测量；每个 client 自己记录 latency vector，结束后 merge，避免在测量
// 热路径里抢全局锁。QPS 严格定义为「窗口内完成 query 数 / 真实 elapsed 秒数」。
//
// P99 需要足够样本：query_count < 1000 时只报告 P50/P95，并明确标记
// "insufficient samples (p99 not reliable)"。
//
// 正确性：测量前对每条 workload 跑一次 engine / prepared / materialized，
// 用与行序无关的 ResultDigest（row_count + hash1 + hash2）比较，任何不一致
// 直接 abort，不做性能测量。
//
// 固定数据集 perf_data（无主键/索引，保证走 SeqScan）：
//     id BIGINT, group_low BIGINT, group_high BIGINT, value DOUBLE, value2 DOUBLE
//     group_low  = row % 1024
//     group_high = row % 65536
//     value      ~ U(0,1)        value2 ~ U(0,1000)
//     固定种子 20260919
//
// 固定 workload：
//     Q1 storage SIMD compare        SELECT COUNT(*) WHERE value > 0.5
//     Q2 execution Filter SIMD       SELECT COUNT(*) WHERE value >= 0.5
//     Q3 sparse projection / gather  SELECT id, value + 1.0 WHERE value > 0.999
//     Q4 低基数 GROUP BY              GROUP BY group_low  (1024 groups)
//     Q5 高基数 GROUP BY              GROUP BY group_high (65536 groups)
//     mixed                          Q1 25% / Q2 25% / Q3 15% / Q4 20% / Q5 15%
//
// AVX2 A/B 与内存消融都由本程序同一二进制、同一份数据、独立进程完成：
//   scalar: SIMPLE_OLAP_FORCE_SCALAR=1 ./bench_query_workload ...
//   avx2  : ./bench_query_workload ...
//   M0..M3: --memory system|arena × --buffer-mode direct|pooled
//
// 使用：
//   ./build/bin/bench_query_workload --db <dir> --prepare --rows 4000000
//   ./build/bin/bench_query_workload --db <dir> --workload mixed --clients 4 \
//       --result-mode both --warmup 10 --duration 60 --csv results.csv

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <memory>
#include <random>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "../src/catalog/table_catalog_entry.h"
#include "../src/execution/expression/exec_expression.h"
#include "../src/main/connection.h"
#include "../src/main/database.h"
#include "../src/main/query_result.h"
#include "../src/main/result_digest.h"
#include "../src/simd/kernels.h"
#include "../src/storage/datachunk.h"
#include "../src/storage/storage_manager.h"
#include "../src/storage/table/table_storage.h"
#include "../src/type.h"
#include "bench_common.h"

#ifndef SIMPLE_OLAP_BENCH_ROOT_DIR
#define SIMPLE_OLAP_BENCH_ROOT_DIR "."
#endif

using namespace simple_olap;
using namespace simple_olap::bench;

namespace {

constexpr const char* kTableName = "perf_data";
constexpr uint32_t kWriteBatchRows = 8192;

// ==========================================================
// Workload 定义
// ==========================================================

enum class Workload : uint8_t {
    Q1 = 0, // storage SIMD compare
    Q2,     // execution filter SIMD
    Q3,     // sparse projection / gather
    Q4,     // 低基数 GROUP BY
    Q5,     // 高基数 GROUP BY
    MIXED,
    COUNT,
};

constexpr size_t kWorkloadKindCount = static_cast<size_t>(Workload::MIXED); // 不含 MIXED 本身

const char* WorkloadName(Workload w) {
    switch (w) {
    case Workload::Q1:
        return "q1_storage_filter";
    case Workload::Q2:
        return "q2_exec_filter";
    case Workload::Q3:
        return "q3_sparse_projection";
    case Workload::Q4:
        return "q4_group_low";
    case Workload::Q5:
        return "q5_group_high";
    case Workload::MIXED:
        return "mixed";
    default:
        return "unknown";
    }
}

using SqlTexts = std::array<std::string, kWorkloadKindCount>;

SqlTexts MakeSqlTexts() {
    SqlTexts texts;
    texts[static_cast<size_t>(Workload::Q1)] = "SELECT COUNT(*) FROM perf_data WHERE value > 0.5;";
    texts[static_cast<size_t>(Workload::Q2)] = "SELECT COUNT(*) FROM perf_data WHERE value >= 0.5;";
    texts[static_cast<size_t>(Workload::Q3)] = "SELECT id, value + 1.0 FROM perf_data WHERE value > 0.999;";
    texts[static_cast<size_t>(Workload::Q4)] =
        "SELECT group_low, COUNT(*), SUM(value) FROM perf_data WHERE value > 0.2 GROUP BY group_low;";
    texts[static_cast<size_t>(Workload::Q5)] =
        "SELECT group_high, COUNT(*), SUM(value) FROM perf_data GROUP BY group_high;";
    return texts;
}

// mixed 固定 schedule：Q1 25 / Q2 25 / Q3 15 / Q4 20 / Q5 15，长度 100。
std::vector<Workload> BuildMixedSchedule() {
    std::vector<Workload> schedule;
    schedule.reserve(100);
    const auto append = [&schedule](Workload w, size_t count) {
        for (size_t i = 0; i < count; ++i) {
            schedule.push_back(w);
        }
    };
    append(Workload::Q1, 25);
    append(Workload::Q2, 25);
    append(Workload::Q3, 15);
    append(Workload::Q4, 20);
    append(Workload::Q5, 15);
    return schedule;
}

bool ParseWorkload(const std::string& text, Workload& out) {
    if (text == "q1") {
        out = Workload::Q1;
    } else if (text == "q2") {
        out = Workload::Q2;
    } else if (text == "q3") {
        out = Workload::Q3;
    } else if (text == "q4") {
        out = Workload::Q4;
    } else if (text == "q5") {
        out = Workload::Q5;
    } else if (text == "mixed") {
        out = Workload::MIXED;
    } else {
        return false;
    }
    return true;
}

std::vector<Workload> BuildSchedule(Workload workload) {
    if (workload == Workload::MIXED) {
        return BuildMixedSchedule();
    }
    return {workload};
}

// ==========================================================
// 结果模式
// ==========================================================

enum class ResultMode : uint8_t {
    ENGINE,       // Connection::Execute(sql, consumer)
    PREPARED,     // Connection::Execute(PreparedQuery, consumer)
    MATERIALIZED, // Connection::Query(sql)
};

const char* ResultModeName(ResultMode mode) {
    switch (mode) {
    case ResultMode::ENGINE:
        return "engine";
    case ResultMode::PREPARED:
        return "prepared";
    case ResultMode::MATERIALIZED:
        return "materialized";
    }
    return "unknown";
}

bool ParseResultModeList(const std::string& text, std::vector<ResultMode>& out) {
    if (text == "engine") {
        out = {ResultMode::ENGINE};
    } else if (text == "prepared") {
        out = {ResultMode::PREPARED};
    } else if (text == "materialized") {
        out = {ResultMode::MATERIALIZED};
    } else if (text == "both") {
        out = {ResultMode::ENGINE, ResultMode::MATERIALIZED};
    } else if (text == "all") {
        out = {ResultMode::ENGINE, ResultMode::PREPARED, ResultMode::MATERIALIZED};
    } else {
        return false;
    }
    return true;
}

// ==========================================================
// 参数解析
// ==========================================================

struct Args {
    BenchOptions opts;
    Workload workload = Workload::MIXED;
    std::vector<ResultMode> result_modes{ResultMode::ENGINE, ResultMode::MATERIALIZED};
    bool verify = true;
    uint32_t load_batch_rows = kWriteBatchRows;
    std::string csv_path;
};

void PrintUsage() {
    std::cout <<
        "Usage: bench_query_workload [options]\n"
        "\n"
        "  --db DIR              数据库目录（默认 <repo>/benchmark/bench_data/perf_db）\n"
        "  --prepare             创建/重建 perf_data 并写入 --rows 行数据\n"
        "  --rows N              数据集行数（默认 4000000）\n"
        "  --load-batch N        数据写入侧 DataChunk 行数（默认 8192）\n"
        "  --workload NAME       q1|q2|q3|q4|q5|mixed（默认 mixed）\n"
        "  --result-mode MODE    engine|prepared|materialized|both|all（默认 both）\n"
        "  --no-verify           跳过 engine/materialized/prepared 摘要一致性校验\n"
        "  --clients N           并发 client 数（默认 1）\n"
        "  --warmup S            每个模式预热秒数（默认 5，0 表示跳过）\n"
        "  --duration S          每个模式测量秒数（默认 60）\n"
        "  --min-samples N       样本不足时自动追加测量轮次，直到达到 N（默认 2000）\n"
        "  --mode MODE           single|multi|auto（默认 single；client 并发固定用 single）\n"
        "  --threads N           pipeline worker 数（morsel-driven，单查询并行度）\n"
        "  --query-workers N     --threads 的别名（只影响单查询并行度）\n"
        "  --queue N             结果交接队列容量（非聚合 SELECT，默认 16）\n"
        "  --memory MODE         arena|system（默认 arena）\n"
        "  --buffer-mode MODE    pooled|direct（默认 pooled）\n"
        "  --block-cache N       BlockPool max_cached_blocks（默认 64）\n"
        "  --buffer-cache N      BufferPool max_cached_per_class（默认 64）\n"
        "  --csv PATH            追加一行 CSV 结果（文件不存在时写表头）\n"
        "  -h, --help            显示帮助\n";
}

bool ParseExecutionMode(const std::string& text, ExecutionMode& out) {
    if (text == "single") {
        out = ExecutionMode::SINGLE_THREAD;
    } else if (text == "multi") {
        out = ExecutionMode::MULTI_THREAD;
    } else if (text == "auto") {
        out = ExecutionMode::AUTO;
    } else {
        return false;
    }
    return true;
}

Args ParseArgs(int argc, char** argv) {
    Args args;
    args.opts.db_path = std::filesystem::path(SIMPLE_OLAP_BENCH_ROOT_DIR) / "benchmark/bench_data/perf_db";

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        const auto next = [&](const char* name) -> std::string {
            if (i + 1 >= argc) {
                throw std::runtime_error(std::string("missing value for ") + name);
            }
            return argv[++i];
        };

        if (arg == "--db") {
            args.opts.db_path = next("--db");
        } else if (arg == "--prepare") {
            args.opts.prepare_data = true;
        } else if (arg == "--rows") {
            args.opts.rows = std::stoull(next("--rows"));
        } else if (arg == "--load-batch") {
            args.load_batch_rows = static_cast<uint32_t>(std::stoul(next("--load-batch")));
        } else if (arg == "--workload") {
            if (!ParseWorkload(next("--workload"), args.workload)) {
                throw std::runtime_error("invalid --workload");
            }
        } else if (arg == "--result-mode") {
            if (!ParseResultModeList(next("--result-mode"), args.result_modes)) {
                throw std::runtime_error("invalid --result-mode");
            }
        } else if (arg == "--no-verify") {
            args.verify = false;
        } else if (arg == "--clients") {
            args.opts.clients = std::stoul(next("--clients"));
        } else if (arg == "--warmup") {
            args.opts.warmup_seconds = static_cast<uint32_t>(std::stoul(next("--warmup")));
        } else if (arg == "--duration") {
            args.opts.duration_seconds = static_cast<uint32_t>(std::stoul(next("--duration")));
        } else if (arg == "--min-samples") {
            args.opts.min_samples = std::stoull(next("--min-samples"));
        } else if (arg == "--mode") {
            if (!ParseExecutionMode(next("--mode"), args.opts.execution_mode)) {
                throw std::runtime_error("invalid --mode");
            }
        } else if (arg == "--threads" || arg == "--query-workers") {
            args.opts.worker_threads = std::stoul(next("--threads"));
        } else if (arg == "--queue") {
            args.opts.result_queue_capacity = std::stoul(next("--queue"));
        } else if (arg == "--memory") {
            const std::string mode = next("--memory");
            if (mode == "arena") {
                args.opts.memory_mode = QueryMemoryMode::ARENA;
            } else if (mode == "system") {
                args.opts.memory_mode = QueryMemoryMode::SYSTEM;
            } else {
                throw std::runtime_error("invalid --memory");
            }
        } else if (arg == "--buffer-mode") {
            const std::string mode = next("--buffer-mode");
            if (mode == "pooled") {
                args.opts.buffer_pool_mode = BufferPoolMode::POOLED;
            } else if (mode == "direct") {
                args.opts.buffer_pool_mode = BufferPoolMode::DIRECT;
            } else {
                throw std::runtime_error("invalid --buffer-mode");
            }
        } else if (arg == "--block-cache") {
            args.opts.block_pool_cache = std::stoul(next("--block-cache"));
        } else if (arg == "--buffer-cache") {
            args.opts.buffer_pool_cache = std::stoul(next("--buffer-cache"));
        } else if (arg == "--csv") {
            args.csv_path = next("--csv");
        } else if (arg == "-h" || arg == "--help") {
            PrintUsage();
            std::exit(0);
        } else {
            throw std::runtime_error("unknown argument: " + arg);
        }
    }

    if (args.opts.clients == 0) {
        args.opts.clients = 1;
    }
    if (args.load_batch_rows == 0) {
        args.load_batch_rows = kWriteBatchRows;
    }
    return args;
}

// ==========================================================
// 数据集准备
// ==========================================================

TableSchema MakePerfSchema() {
    TableSchema schema;
    schema.columns.push_back(ColumnSchema{0, "id", DataType::INT64});
    schema.columns.push_back(ColumnSchema{1, "group_low", DataType::INT64});
    schema.columns.push_back(ColumnSchema{2, "group_high", DataType::INT64});
    schema.columns.push_back(ColumnSchema{3, "value", DataType::DOUBLE});
    schema.columns.push_back(ColumnSchema{4, "value2", DataType::DOUBLE});
    return schema;
}

TableStorage* OpenPerfTable(Database& database) {
    const auto table_id = database.GetCatalog().FindTable(kTableName);
    if (!table_id.has_value()) {
        return nullptr;
    }
    const TableCatalogEntry* entry = database.GetCatalog().GetTable(*table_id);
    if (entry == nullptr) {
        return nullptr;
    }
    auto storage = database.GetStorageManager().GetTable(*table_id, entry->schema);
    return storage.get();
}

bool FillDataset(TableStorage* table, uint64_t rows, uint32_t load_batch_rows) {
    std::mt19937_64 rng(20260919ULL); // 固定种子，保证可复现
    std::uniform_real_distribution<double> value_dist(0.0, 1.0);
    std::uniform_real_distribution<double> value2_dist(0.0, 1000.0);

    uint64_t written = 0;
    while (written < rows) {
        const uint32_t n = static_cast<uint32_t>(std::min<uint64_t>(load_batch_rows, rows - written));

        auto id_buf = std::shared_ptr<uint8_t[]>(new uint8_t[static_cast<size_t>(n) * sizeof(int64_t)]);
        auto low_buf = std::shared_ptr<uint8_t[]>(new uint8_t[static_cast<size_t>(n) * sizeof(int64_t)]);
        auto high_buf = std::shared_ptr<uint8_t[]>(new uint8_t[static_cast<size_t>(n) * sizeof(int64_t)]);
        auto value_buf = std::shared_ptr<uint8_t[]>(new uint8_t[static_cast<size_t>(n) * sizeof(double)]);
        auto value2_buf = std::shared_ptr<uint8_t[]>(new uint8_t[static_cast<size_t>(n) * sizeof(double)]);

        auto* ids = reinterpret_cast<int64_t*>(id_buf.get());
        auto* lows = reinterpret_cast<int64_t*>(low_buf.get());
        auto* highs = reinterpret_cast<int64_t*>(high_buf.get());
        auto* values = reinterpret_cast<double*>(value_buf.get());
        auto* values2 = reinterpret_cast<double*>(value2_buf.get());

        for (uint32_t i = 0; i < n; ++i) {
            const uint64_t row = written + i;
            ids[i] = static_cast<int64_t>(row);
            lows[i] = static_cast<int64_t>(row % 1024);
            highs[i] = static_cast<int64_t>(row % 65536);
            values[i] = value_dist(rng);
            values2[i] = value2_dist(rng);
        }

        DataChunk chunk(n);
        chunk.set_data(0, id_buf, sizeof(int64_t));
        chunk.set_data(1, low_buf, sizeof(int64_t));
        chunk.set_data(2, high_buf, sizeof(int64_t));
        chunk.set_data(3, value_buf, sizeof(double));
        chunk.set_data(4, value2_buf, sizeof(double));
        table->Append(chunk);

        written += n;
    }

    return table->Flush();
}

bool EnsureDataset(Database& database, const Args& args) {
    const BenchOptions& opts = args.opts;
    const bool exists = database.GetCatalog().FindTable(kTableName).has_value();

    if (!opts.prepare_data) {
        return exists;
    }

    // --prepare：重建数据集，保证行数与内容可复现。
    if (exists && !database.DropTable(kTableName)) {
        std::cerr << "failed to drop existing " << kTableName << "\n";
        return false;
    }
    if (!database.CreateTable(kTableName, MakePerfSchema())) {
        std::cerr << "failed to create " << kTableName << "\n";
        return false;
    }

    TableStorage* table = OpenPerfTable(database);
    if (table == nullptr) {
        std::cerr << "failed to open " << kTableName << "\n";
        return false;
    }

    std::cout << "preparing dataset: " << kTableName << " rows=" << opts.rows << " load_batch=" << args.load_batch_rows
              << " ..." << std::flush;
    if (!FillDataset(table, opts.rows, args.load_batch_rows)) {
        std::cerr << "\nfailed to flush dataset\n";
        return false;
    }
    std::cout << " done\n";
    return true;
}

// ==========================================================
// 正确性：三种路径的 ResultDigest 必须一致
// ==========================================================

ResultDigest DigestEngine(Connection& connection, std::string_view sql) {
    ResultDigest digest;
    connection.Execute(sql, [&](const VectorBatch& batch, const ExecSchema&) { AccumulateBatch(digest, batch); });
    return digest;
}

ResultDigest DigestPrepared(Connection& connection, PreparedQuery& prepared) {
    ResultDigest digest;
    connection.Execute(prepared,
                       [&](const VectorBatch& batch, const ExecSchema&) { AccumulateBatch(digest, batch); });
    return digest;
}

ResultDigest DigestMaterialized(Connection& connection, std::string_view sql) {
    const QueryResult result = connection.Query(sql);
    return ComputeResultDigest(result);
}

bool VerifyDigests(Database& database, const std::vector<Workload>& schedule, const SqlTexts& sql_texts,
                   const std::vector<ResultMode>& modes, bool verbose) {
    Connection connection(database);

    // schedule 里同一 workload 会出现多次，digest 校验只需每种 workload 一次。
    std::vector<Workload> unique_workloads;
    for (Workload workload : schedule) {
        if (std::find(unique_workloads.begin(), unique_workloads.end(), workload) == unique_workloads.end()) {
            unique_workloads.push_back(workload);
        }
    }

    for (Workload workload : unique_workloads) {
        const std::string& sql = sql_texts[static_cast<size_t>(workload)];

        const ResultDigest engine = DigestEngine(connection, sql);
        const ResultDigest materialized = DigestMaterialized(connection, sql);

        if (!SameDigest(engine, materialized)) {
            std::printf("correctness FAIL: %s engine vs materialized digest mismatch "
                        "(rows %llu/%llu, hash1 %llu/%llu)\n",
                        WorkloadName(workload), static_cast<unsigned long long>(engine.row_count),
                        static_cast<unsigned long long>(materialized.row_count),
                        static_cast<unsigned long long>(engine.hash1),
                        static_cast<unsigned long long>(materialized.hash1));
            return false;
        }

        if (std::find(modes.begin(), modes.end(), ResultMode::PREPARED) != modes.end()) {
            PreparedQuery prepared = connection.Prepare(sql);
            const ResultDigest prepared_digest = DigestPrepared(connection, prepared);
            if (!SameDigest(engine, prepared_digest)) {
                std::printf("correctness FAIL: %s engine vs prepared digest mismatch "
                            "(rows %llu/%llu, hash1 %llu/%llu)\n",
                            WorkloadName(workload), static_cast<unsigned long long>(engine.row_count),
                            static_cast<unsigned long long>(prepared_digest.row_count),
                            static_cast<unsigned long long>(engine.hash1),
                            static_cast<unsigned long long>(prepared_digest.hash1));
                return false;
            }
        }

        if (verbose) {
            std::printf("verify: %-22s rows=%llu hash1=%llu [OK]\n", WorkloadName(workload),
                        static_cast<unsigned long long>(engine.row_count),
                        static_cast<unsigned long long>(engine.hash1));
        }
    }
    return true;
}

// ==========================================================
// client 执行 + 计时
// ==========================================================

struct ClientOutput {
    std::vector<uint64_t> latencies_ns;
    uint64_t sink = 0; // 防止结果被优化掉；O(1)/batch
};

void ClientLoop(Database& database, const Workload* schedule, size_t schedule_size, const SqlTexts& sql_texts,
                ResultMode mode, std::atomic<bool>& start, std::atomic<bool>& stop, ClientOutput& output) {
    Connection connection(database);

    // prepared 模式：planning 在测量窗口之前完成，不属于被测延迟。
    std::vector<PreparedQuery> prepared;
    if (mode == ResultMode::PREPARED) {
        prepared.reserve(schedule_size);
        for (size_t i = 0; i < schedule_size; ++i) {
            prepared.push_back(connection.Prepare(sql_texts[static_cast<size_t>(schedule[i])]));
        }
    }

    // 黑盒 consumer：只做 O(1) 累加，不物化、不遍历结果行。
    const BatchConsumer blackhole = [&output](const VectorBatch& batch, const ExecSchema&) {
        output.sink += ActiveRowCount(batch);
    };

    while (!start.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }

    size_t cursor = 0;
    while (!stop.load(std::memory_order_relaxed)) {
        const size_t schedule_index = cursor;
        const Workload workload = schedule[schedule_index];
        cursor = (cursor + 1) % schedule_size;

        const uint64_t begin = NowNs();
        switch (mode) {
        case ResultMode::ENGINE: {
            connection.Execute(sql_texts[static_cast<size_t>(workload)], blackhole);
            break;
        }
        case ResultMode::PREPARED: {
            connection.Execute(prepared[schedule_index], blackhole);
            break;
        }
        case ResultMode::MATERIALIZED: {
            QueryResult result = connection.Query(sql_texts[static_cast<size_t>(workload)]);
            const uint64_t end = NowNs();
            output.sink += result.affected_rows;
            output.latencies_ns.push_back(end - begin);
            result = QueryResult{}; // 下一条 query 前释放，保证 BufferPool 复用
            continue;
        }
        }
        const uint64_t end = NowNs();

        output.latencies_ns.push_back(end - begin);
    }
}

struct MeasurementResult {
    std::vector<uint64_t> latencies_ns;
    double elapsed_seconds = 0.0;
    uint64_t sink = 0;
    uint32_t rounds = 0;
};

// 运行一轮（或样本不足时多轮）测量：
//   每轮创建 clients 个线程，统一 start 后跑 seconds 秒，stop 后 join。
MeasurementResult RunMeasurement(Database& database, const std::vector<Workload>& schedule, const SqlTexts& sql_texts,
                                 const BenchOptions& opts, ResultMode mode, uint32_t seconds, uint64_t min_samples) {
    MeasurementResult result;
    constexpr uint32_t kMaxRounds = 20;
    const size_t clients = std::max<size_t>(1, opts.clients);
    const uint32_t round_seconds = std::max<uint32_t>(1, seconds);

    for (uint32_t round = 0; round < kMaxRounds; ++round) {
        std::vector<ClientOutput> outputs(clients);
        std::atomic<bool> start{false};
        std::atomic<bool> stop{false};

        std::vector<std::thread> threads;
        threads.reserve(clients);
        for (size_t i = 0; i < clients; ++i) {
            threads.emplace_back(ClientLoop, std::ref(database), schedule.data(), schedule.size(), std::cref(sql_texts),
                                 mode, std::ref(start), std::ref(stop), std::ref(outputs[i]));
        }

        const uint64_t begin = NowNs();
        start.store(true, std::memory_order_release);
        std::this_thread::sleep_for(std::chrono::seconds(round_seconds));
        stop.store(true, std::memory_order_relaxed);
        for (auto& thread : threads) {
            thread.join();
        }
        const uint64_t end = NowNs();

        result.elapsed_seconds += static_cast<double>(end - begin) / 1e9;
        for (auto& output : outputs) {
            result.latencies_ns.insert(result.latencies_ns.end(), output.latencies_ns.begin(),
                                       output.latencies_ns.end());
            result.sink += output.sink;
        }
        ++result.rounds;

        if (result.latencies_ns.size() >= min_samples) {
            break;
        }
        std::cout << "  [min-samples] " << result.latencies_ns.size() << " < " << min_samples
                  << ", extending measurement by " << round_seconds << "s ...\n";
    }

    return result;
}

// ==========================================================
// 输出
// ==========================================================

const char* kCsvHeader =
    "benchmark,backend,workload,result_mode,rows,clients,execution_mode,worker_threads,memory_mode,"
    "buffer_mode,block_cache,buffer_cache,elapsed_s,queries,qps,mean_us,min_us,p50_us,p95_us,p99_us,max_us,"
    "p99_reliable,block_system_alloc,block_system_free,block_hits,block_returns,"
    "buffer_system_alloc,buffer_system_free,buffer_hits,buffer_returns";

void AppendCsvRow(const std::string& path, const std::string& row) {
    const bool need_header = !std::filesystem::exists(path) || std::filesystem::file_size(path) == 0;

    std::ofstream out(path, std::ios::app);
    if (!out) {
        throw std::runtime_error("cannot open csv file: " + path);
    }
    if (need_header) {
        out << kCsvHeader << "\n";
    }
    out << row << "\n";
}

void PrintModeResult(ResultMode mode, const LatencyStats& stats, const MemoryStats& memory) {
    std::printf("%-13s qps=%10.2f  p50=%8lluus p95=%8lluus p99=%8lluus mean=%8.1fus min=%7lluus max=%9lluus",
                ResultModeName(mode), stats.qps, static_cast<unsigned long long>(stats.p50_us),
                static_cast<unsigned long long>(stats.p95_us), static_cast<unsigned long long>(stats.p99_us),
                stats.mean_us, static_cast<unsigned long long>(stats.min_us),
                static_cast<unsigned long long>(stats.max_us));
    if (!stats.p99_reliable()) {
        std::printf("  [insufficient samples for p99: N=%llu < %llu]",
                    static_cast<unsigned long long>(stats.query_count),
                    static_cast<unsigned long long>(kMinSamplesForP99));
    }
    std::printf("\n             queries=%llu elapsed=%.3fs alloc/query=%.2f pool_hit_rate=%.4f\n",
                static_cast<unsigned long long>(stats.query_count), stats.elapsed_seconds,
                stats.query_count == 0
                    ? 0.0
                    : static_cast<double>(memory.block_system_alloc() + memory.buffer_system_alloc()) /
                          static_cast<double>(stats.query_count),
                memory.pool_hit_rate());
}

} // namespace

int main(int argc, char** argv) {
    Args args;
    try {
        args = ParseArgs(argc, argv);
    } catch (const std::exception& error) {
        std::cerr << "argument error: " << error.what() << "\n";
        PrintUsage();
        return 2;
    }

    const BenchOptions& opts = args.opts;

    DatabaseConfig config;
    config.block_pool_max_cached_blocks = opts.block_pool_cache;
    config.buffer_pool_max_cached_per_class = opts.buffer_pool_cache;
    config.query_memory_mode = opts.memory_mode;
    config.buffer_pool_mode = opts.buffer_pool_mode;
    config.execution_mode = opts.execution_mode;
    config.parallel_config.worker_threads = std::max<size_t>(1, opts.worker_threads);
    config.parallel_config.result_queue_capacity = std::max<size_t>(1, opts.result_queue_capacity);
    // 线程池必须至少能同时容纳全部 pipeline worker
    config.thread_count = std::max(config.thread_count, opts.worker_threads);

    Database database(opts.db_path, config);

    if (!EnsureDataset(database, args)) {
        std::cerr << "dataset " << kTableName << " not found under " << opts.db_path
                  << " (use --prepare to generate it)\n";
        return 1;
    }

    // 正式测量前把表打开一次：StorageManager::tables_ 无锁，
    // 避免多个 client 线程同时触发 lazy open。
    TableStorage* table = OpenPerfTable(database);
    if (table == nullptr) {
        std::cerr << "failed to pre-open " << kTableName << "\n";
        return 1;
    }
    const size_t segment_count = table->segment_count();

    const std::vector<Workload> schedule = BuildSchedule(args.workload);
    const SqlTexts sql_texts = MakeSqlTexts();

    // ---------- 环境 ----------
    const EnvironmentInfo env = QueryEnvironment();
    const std::string backend =
        simd::KernelRegistry::Instance().backend() == simd::SimdBackend::AVX2 ? "avx2" : "scalar";

    std::printf("=== bench_query_workload ===\n");
    PrintEnvironment(env);
    std::printf("config: backend=%s workload=%s rows=%llu segments=%zu load_batch=%u clients=%zu execution=%s "
                "workers=%zu queue=%zu memory=%s buffer=%s block_cache=%zu buffer_cache=%zu\n",
                backend.c_str(), WorkloadName(args.workload), static_cast<unsigned long long>(opts.rows),
                segment_count, args.load_batch_rows, opts.clients, ExecutionModeName(opts.execution_mode),
                config.parallel_config.worker_threads, config.parallel_config.result_queue_capacity,
                MemoryModeName(opts.memory_mode), BufferPoolModeName(opts.buffer_pool_mode), opts.block_pool_cache,
                opts.buffer_pool_cache);
    std::printf("measure: warmup=%us duration=%us min_samples=%llu result_modes=", opts.warmup_seconds,
                opts.duration_seconds, static_cast<unsigned long long>(opts.min_samples));
    for (size_t i = 0; i < args.result_modes.size(); ++i) {
        std::printf("%s%s", i == 0 ? "" : "+", ResultModeName(args.result_modes[i]));
    }
    std::printf("\n\n");
    std::fflush(stdout);

    // ---------- 正确性：三种路径 digest 必须一致 ----------
    if (args.verify) {
        if (!VerifyDigests(database, schedule, sql_texts, args.result_modes, /*verbose=*/true)) {
            std::cerr << "correctness verification failed; benchmark invalid\n";
            return 1;
        }
        std::printf("\n");
    }

    // ---------- warmup（统一做一次，结果丢弃） ----------
    if (opts.warmup_seconds > 0) {
        const MeasurementResult warmup =
            RunMeasurement(database, schedule, sql_texts, opts, args.result_modes.front(), opts.warmup_seconds, 0);
        std::printf("warmup: queries=%llu elapsed=%.3fs sink=%llu\n\n",
                    static_cast<unsigned long long>(warmup.latencies_ns.size()), warmup.elapsed_seconds,
                    static_cast<unsigned long long>(warmup.sink));
        std::fflush(stdout);
    }

    // ---------- 每个模式测量 + snapshot ----------
    for (ResultMode mode : args.result_modes) {
        MemoryStats memory;
        memory.block_before = database.GetBlockPool().stats();
        memory.buffer_before = database.GetBufferPool().stats();

        const MeasurementResult measurement =
            RunMeasurement(database, schedule, sql_texts, opts, mode, opts.duration_seconds, opts.min_samples);

        memory.block_after = database.GetBlockPool().stats();
        memory.buffer_after = database.GetBufferPool().stats();

        const LatencyStats stats = ComputeLatencyStats(measurement.latencies_ns, measurement.elapsed_seconds);
        PrintModeResult(mode, stats, memory);

        if (!args.csv_path.empty()) {
            char row[2048];
            std::snprintf(row, sizeof(row),
                          "query_workload,%s,%s,%s,%llu,%zu,%s,%zu,%s,%s,%zu,%zu,%.3f,%llu,%.2f,%.1f,%llu,%llu,"
                          "%llu,%llu,%llu,%d,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu",
                          backend.c_str(), WorkloadName(args.workload), ResultModeName(mode),
                          static_cast<unsigned long long>(opts.rows), opts.clients,
                          ExecutionModeName(opts.execution_mode), config.parallel_config.worker_threads,
                          MemoryModeName(opts.memory_mode),
                          BufferPoolModeName(opts.buffer_pool_mode), opts.block_pool_cache, opts.buffer_pool_cache,
                          stats.elapsed_seconds, static_cast<unsigned long long>(stats.query_count), stats.qps,
                          stats.mean_us, static_cast<unsigned long long>(stats.min_us),
                          static_cast<unsigned long long>(stats.p50_us),
                          static_cast<unsigned long long>(stats.p95_us),
                          static_cast<unsigned long long>(stats.p99_us),
                          static_cast<unsigned long long>(stats.max_us), stats.p99_reliable() ? 1 : 0,
                          static_cast<unsigned long long>(memory.block_system_alloc()),
                          static_cast<unsigned long long>(memory.block_system_free()),
                          static_cast<unsigned long long>(memory.block_hits()),
                          static_cast<unsigned long long>(memory.block_returns()),
                          static_cast<unsigned long long>(memory.buffer_system_alloc()),
                          static_cast<unsigned long long>(memory.buffer_system_free()),
                          static_cast<unsigned long long>(memory.buffer_hits()),
                          static_cast<unsigned long long>(memory.buffer_returns()));
            AppendCsvRow(args.csv_path, row);
        }
    }

    // ---------- 测量后再次校验：防止 PreparedQuery 复用 / BufferPool 复用
    //            导致结果在长时间运行后发生变化 ----------
    if (args.verify) {
        if (!VerifyDigests(database, schedule, sql_texts, args.result_modes, /*verbose=*/false)) {
            std::cerr << "post-measurement correctness verification failed; benchmark invalid\n";
            return 1;
        }
        std::printf("post-measurement verify: all digests still agree [OK]\n");
    }

    return 0;
}
