#pragma once

// ============================================================
// Compare.md benchmark protocol（simple_olap / DuckDB 共用）
//
// 固定约定（见 docs/Compare.md）：
//   - 数据集逐行一致：id=i, grp=i%1024, grp_hi=i%65536,
//     value=((i*48271)%1000003)/1000003.0
//   - QPS = 完成查询数 / 总墙钟时间（不用 1/median）
//   - 每个查询先 warmup 再计时，逐次记录 latency，重新算 p50/p95/p99
//   - 结果必须被真正消费（order-independent digest），防止查询被优化掉
//   - 输出统一 CSV，附 context switch 计数（getrusage）
// ============================================================

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace olap_compare {

using Clock = std::chrono::steady_clock;

inline uint64_t NowNs() {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count());
}

// ---------- 数据集（Compare.md §8） ----------

constexpr uint64_t kValueMultiplier = 48271ULL;
constexpr uint64_t kValueModulus = 1000003ULL;

inline double DeterministicValue(uint64_t row) {
    return static_cast<double>((row * kValueMultiplier) % kValueModulus) / static_cast<double>(kValueModulus);
}

struct QuerySpec {
    const char* id;
    const char* name;
    const char* sql;
};

// Compare.md §10-§16 的 7 条能力交集查询。
inline const std::vector<QuerySpec>& Queries() {
    static const std::vector<QuerySpec> queries = {
        {"q1", "global_agg", "SELECT SUM(value) FROM bench;"},
        {"q2", "filter_50_agg", "SELECT COUNT(*), SUM(value) FROM bench WHERE value > 0.5;"},
        {"q3", "filter_90_agg", "SELECT COUNT(*), SUM(value) FROM bench WHERE value > 0.1;"},
        {"q4", "filter_99_agg", "SELECT COUNT(*), SUM(value) FROM bench WHERE value > 0.99;"},
        {"q5", "expr_agg", "SELECT SUM(id + 1) FROM bench;"},
        {"q6", "group_low", "SELECT grp, COUNT(*), SUM(value) FROM bench GROUP BY grp;"},
        {"q7", "group_high", "SELECT grp_hi, COUNT(*), SUM(value) FROM bench GROUP BY grp_hi;"},
    };
    return queries;
}

// ---------- CLI ----------

struct CliOptions {
    std::string engine = "unknown";
    std::string commit = "unknown";
    std::string dataset = "medium";
    std::string db_path;
    std::string query = "all";
    size_t threads = 1;
    double warmup_seconds = 2.0;
    double measure_seconds = 5.0;
    std::string csv_path;

    bool verify = false;
    bool generate = false;
    uint64_t rows = 0;
};

inline void PrintUsage(const char* argv0) {
    std::printf(
        "usage: %s [options]\n"
        "  --engine NAME      engine label (simple_direct|simple_queue|duckdb)\n"
        "  --commit HASH      engine commit/tag\n"
        "  --dataset NAME     small|medium|large\n"
        "  --db PATH          database directory/file\n"
        "  --query ID         all | q1..q7 (default all)\n"
        "  --threads N        worker threads (DuckDB: SET threads=N)\n"
        "  --warmup S         warmup seconds per query (default 2)\n"
        "  --measure S        measurement seconds per query (default 5)\n"
        "  --csv PATH         append result rows to CSV\n"
        "  --generate         generate dataset and exit\n"
        "  --rows N           rows for --generate\n"
        "  --verify           run each query once, print digest, exit\n",
        argv0);
}

inline CliOptions ParseCli(int argc, char** argv) {
    CliOptions opt;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        const auto next = [&](const char* name) -> std::string {
            if (i + 1 >= argc) {
                throw std::runtime_error(std::string("missing value for ") + name);
            }
            return argv[++i];
        };

        if (arg == "--engine") {
            opt.engine = next("--engine");
        } else if (arg == "--commit") {
            opt.commit = next("--commit");
        } else if (arg == "--dataset") {
            opt.dataset = next("--dataset");
        } else if (arg == "--db") {
            opt.db_path = next("--db");
        } else if (arg == "--query") {
            opt.query = next("--query");
        } else if (arg == "--threads") {
            opt.threads = static_cast<size_t>(std::stoul(next("--threads")));
        } else if (arg == "--warmup") {
            opt.warmup_seconds = std::stod(next("--warmup"));
        } else if (arg == "--measure") {
            opt.measure_seconds = std::stod(next("--measure"));
        } else if (arg == "--csv") {
            opt.csv_path = next("--csv");
        } else if (arg == "--rows") {
            opt.rows = std::stoull(next("--rows"));
        } else if (arg == "--verify") {
            opt.verify = true;
        } else if (arg == "--generate") {
            opt.generate = true;
        } else if (arg == "-h" || arg == "--help") {
            PrintUsage(argv[0]);
            std::exit(0);
        } else {
            throw std::runtime_error("unknown argument: " + arg);
        }
    }
    if (opt.threads == 0) {
        opt.threads = 1;
    }
    return opt;
}

inline std::vector<const QuerySpec*> SelectQueries(const std::string& selection) {
    std::vector<const QuerySpec*> out;
    for (const QuerySpec& q : Queries()) {
        if (selection == "all" || selection == q.id || selection == q.name) {
            out.push_back(&q);
        }
    }
    if (out.empty()) {
        throw std::runtime_error("unknown --query: " + selection);
    }
    return out;
}

// ---------- 统计 ----------

struct LatencyStats {
    uint64_t iterations = 0;
    double seconds = 0.0;
    double qps = 0.0;
    double mean_us = 0.0;
    double p50_us = 0.0;
    double p95_us = 0.0;
    double p99_us = 0.0;
    double min_us = 0.0;
    double max_us = 0.0;
};

inline double PercentileUs(const std::vector<uint64_t>& sorted_ns, double percentile) {
    if (sorted_ns.empty()) {
        return 0.0;
    }
    size_t index = static_cast<size_t>(std::ceil(percentile / 100.0 * static_cast<double>(sorted_ns.size()))) - 1;
    if (index >= sorted_ns.size()) {
        index = sorted_ns.size() - 1;
    }
    return static_cast<double>(sorted_ns[index]) / 1000.0;
}

inline LatencyStats ComputeStats(std::vector<uint64_t> latencies_ns, double seconds) {
    LatencyStats stats;
    stats.iterations = latencies_ns.size();
    stats.seconds = seconds;
    stats.qps = seconds > 0.0 ? static_cast<double>(stats.iterations) / seconds : 0.0;
    if (latencies_ns.empty()) {
        return stats;
    }

    std::sort(latencies_ns.begin(), latencies_ns.end());

    double total_ns = 0.0;
    for (uint64_t ns : latencies_ns) {
        total_ns += static_cast<double>(ns);
    }
    stats.mean_us = total_ns / static_cast<double>(latencies_ns.size()) / 1000.0;
    stats.min_us = static_cast<double>(latencies_ns.front()) / 1000.0;
    stats.max_us = static_cast<double>(latencies_ns.back()) / 1000.0;
    stats.p50_us = PercentileUs(latencies_ns, 50.0);
    stats.p95_us = PercentileUs(latencies_ns, 95.0);
    stats.p99_us = PercentileUs(latencies_ns, 99.0);
    return stats;
}

// ---------- 结果消费 / 正确性校验 ----------

// splitmix64 finalizer
inline uint64_t MixHash(uint64_t x) {
    x += 0x9e3779b97f4a7c15ULL;
    x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ULL;
    x = (x ^ (x >> 27)) * 0x94d049bb133111ebULL;
    return x ^ (x >> 31);
}

// 与行序无关的结果累加器：
//   - 整数列（COUNT / 分组键 / SUM(int)）精确累加与平方累加
//   - 浮点列（SUM(double)）用 long double 累加，跨引擎按相对容差比较
// 这样既验证了分组集合与计数，又不会被浮点求和顺序差异误报。
struct ResultAccumulator {
    uint64_t rows = 0;
    long double integer_sum = 0.0L;
    long double integer_sq_sum = 0.0L;
    long double double_sum = 0.0L;

    void AddInteger(int64_t value) {
        integer_sum += value;
        integer_sq_sum += static_cast<long double>(value) * static_cast<long double>(value);
    }

    void AddDouble(double value) {
        double_sum += value;
    }

    void EndRow() {
        ++rows;
    }

    // 仅用于证明结果被真正消费（同一查询跨迭代稳定即可）。
    uint64_t Checksum() const {
        uint64_t double_bits = 0;
        const double d = static_cast<double>(double_sum);
        std::memcpy(&double_bits, &d, sizeof(double_bits));
        uint64_t h = MixHash(static_cast<uint64_t>(integer_sum));
        h = MixHash(h ^ static_cast<uint64_t>(integer_sq_sum));
        return MixHash(h ^ double_bits);
    }
};

// ---------- CSV 输出 ----------

inline const char* CsvHeader() {
    return "engine,commit,dataset,rows,query,threads,iterations,seconds,qps,mean_us,p50_us,p95_us,p99_us,min_us,max_us,"
           "rows_out,checksum,voluntary_cs,involuntary_cs";
}

inline void AppendCsv(const std::string& path, const std::string& row) {
    if (path.empty()) {
        return;
    }
    std::ifstream probe(path);
    const bool need_header = !probe.good() || probe.peek() == std::ifstream::traits_type::eof();
    probe.close();

    std::ofstream out(path, std::ios::app);
    if (!out) {
        throw std::runtime_error("cannot open csv: " + path);
    }
    if (need_header) {
        out << CsvHeader() << "\n";
    }
    out << row << "\n";
}

inline std::string FormatBenchmarkRow(const CliOptions& opt, uint64_t rows, const QuerySpec& query,
                                      const LatencyStats& stats, uint64_t rows_out, uint64_t checksum,
                                      uint64_t voluntary_cs, uint64_t involuntary_cs) {
    char buffer[512];
    std::snprintf(buffer, sizeof(buffer), "%s,%s,%s,%llu,%s,%zu,%llu,%.6f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%llu,%llu,%llu,%llu",
                  opt.engine.c_str(), opt.commit.c_str(), opt.dataset.c_str(),
                  static_cast<unsigned long long>(rows), query.id, opt.threads,
                  static_cast<unsigned long long>(stats.iterations), stats.seconds, stats.qps, stats.mean_us,
                  stats.p50_us, stats.p95_us, stats.p99_us, stats.min_us, stats.max_us,
                  static_cast<unsigned long long>(rows_out), static_cast<unsigned long long>(checksum),
                  static_cast<unsigned long long>(voluntary_cs), static_cast<unsigned long long>(involuntary_cs));
    return buffer;
}

} // namespace olap_compare
