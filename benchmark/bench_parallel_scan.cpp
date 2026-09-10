// benchmark/bench_parallel_scan.cpp
//
// 并行扫描（多线程）与串行扫描（单线程）的运行时间对比。
//
// 设计要点：
//   - 串行路径 TableStorage::Scan 与并行路径 ParallelScanSession 共用同一份
//     ScanSegment 逻辑（metadata pruning / batch 读取 / row filtering），
//     因此两者的吞吐差异来自并发度而非算法差异。
//   - 计时前把数据 Flush 落盘，Scan 只读已落盘 segment。
//   - 串行 ScanCursor 把进度（segment 下标）保存在游标自身，因此同一个
//     TableStorage 实例可以被反复、并发地扫描；本基准在计时区间外只 Open
//     一次，串行/并行复用同一实例，排除重复打开的开销与差异。
//   - 计时包含首次懒加载的 mmap，重复多轮取最优/中位数削弱冷启动影响。
//
// 用法：
//   bench_parallel_scan [--rows N] [--repeats R] [--warmup W]
//                       [--threads 1,2,4,8] [--batch B] [--queue Q]
//                       [--scenario scan|filter|all] [--data DIR] [--help]

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <memory>
#include <numeric>
#include <random>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "execution/batch_utils.h"
#include "execution/vector/vector.h"
#include "storage/datachunk.h"
#include "storage/datastructs.h"
#include "storage/scan/parallel_scan_session.h"
#include "storage/table/table_storage.h"
#include "type.h"

using namespace simple_olap;
using Clock = std::chrono::steady_clock;

namespace {

// 基准表：id INT64, value DOUBLE
constexpr TableId kTableId = 1;

struct Options {
    uint64_t rows = 4'000'000;      // 数据集行数
    int repeats = 5;                // 每个配置的计时轮数
    int warmup = 2;                 // 每配置预热轮数
    uint32_t batch_rows = 8192;     // 写入侧 DataChunk 行数
    size_t queue_capacity = 16;     // 并行队列容量（对齐 ParallelConfig）
    std::vector<size_t> threads;    // 待测并行线程数
    std::string scenario = "all";   // scan | filter | all
    std::filesystem::path data_dir; // 落盘目录
};

struct TimingStats {
    uint64_t rows = 0;
    double best_ms = 0.0;
    double median_ms = 0.0;
    double mean_ms = 0.0;
};

TableSchema MakeSchema() {
    TableSchema schema;
    schema.columns.push_back(ColumnSchema{0, "id", DataType::INT64});
    schema.columns.push_back(ColumnSchema{1, "value", DataType::DOUBLE});
    return schema;
}

// 构造扫描选项。with_filter=true 时下推 value > 0.5：
// 由于 value 在 [0,1) 均匀分布，每个 segment 的 min/max 都跨越 0.5，
// metadata 无法剪枝，所有行都会进入逐行过滤，从而把 CPU 工作也放进来对比。
ScanOptions MakeScanOptions(bool with_filter) {
    ScanOptions opts;
    opts.columns = {0, 1};
    if (with_filter) {
        Condition cond;
        cond.column = 1;
        cond.op = CmpOp::GT;
        cond.value = 0.5; // 显式 double，避免 variant 歧义
        opts.predicates.push_back(cond);
    }
    return opts;
}

// 生成并落盘数据集，返回创建好的 TableStorage。
std::unique_ptr<TableStorage> BuildDataset(const std::filesystem::path& tables_root, const TableSchema& schema,
                                           uint64_t rows, uint32_t batch_rows) {
    std::error_code ec;
    std::filesystem::remove_all(tables_root, ec);

    auto table = TableStorage::Create(kTableId, schema, tables_root);
    if (table == nullptr) {
        throw std::runtime_error("failed to create table storage: " + tables_root.string());
    }

    std::mt19937_64 rng(20240910ULL); // 固定种子，保证可复现
    std::uniform_real_distribution<double> dist(0.0, 1.0);

    uint64_t written = 0;
    while (written < rows) {
        const uint32_t n = static_cast<uint32_t>(std::min<uint64_t>(batch_rows, rows - written));

        auto id_buf = std::shared_ptr<uint8_t[]>(new uint8_t[static_cast<size_t>(n) * sizeof(int64_t)]);
        auto val_buf = std::shared_ptr<uint8_t[]>(new uint8_t[static_cast<size_t>(n) * sizeof(double)]);
        auto* ids = reinterpret_cast<int64_t*>(id_buf.get());
        auto* vals = reinterpret_cast<double*>(val_buf.get());

        for (uint32_t i = 0; i < n; ++i) {
            ids[i] = static_cast<int64_t>(written + i);
            vals[i] = dist(rng);
        }

        DataChunk chunk(n);
        chunk.set_data(0, id_buf, sizeof(int64_t));
        chunk.set_data(1, val_buf, sizeof(double));
        table->Append(chunk);

        written += n;
    }

    if (!table->Flush()) {
        throw std::runtime_error("failed to flush dataset");
    }
    return table;
}

// 一次串行全表扫描。ScanCursor 自持进度，同一个 table 可重复调用。
uint64_t RunSerialOnce(TableStorage* table, const ScanOptions& opts) {
    uint64_t rows = 0;
    ScanCursor cursor{};
    VectorBatch batch;
    while (table->Scan(opts, cursor, batch)) {
        rows += ActiveRowCount(batch);
        batch.Reset();
    }
    return rows;
}

// 一次并行全表扫描：scan worker 从队列推 batch，这里只做消费（count）。
uint64_t RunParallelOnce(TableStorage* table, const ScanOptions& opts, size_t threads, size_t queue_capacity) {
    std::shared_ptr<BatchStream> stream = table->CreateParallelScan(opts, threads, queue_capacity);
    std::static_pointer_cast<ParallelScanSession>(stream)->Start(); // 幂等启动 scan worker

    uint64_t rows = 0;
    VectorBatch batch;
    while (stream->Next(batch)) {
        rows += ActiveRowCount(batch);
        batch.Reset();
    }
    return rows;
}

// 预热 + 多轮计时，返回最优/中位/平均耗时（毫秒）与行数。
template <typename Fn> TimingStats TimeIt(Fn&& fn, int warmup, int repeats) {
    uint64_t rows = 0;
    for (int i = 0; i < warmup; ++i) {
        rows = fn();
    }

    std::vector<double> samples;
    samples.reserve(static_cast<size_t>(repeats));
    for (int i = 0; i < repeats; ++i) {
        const auto t0 = Clock::now();
        rows = fn();
        const auto t1 = Clock::now();
        samples.push_back(std::chrono::duration<double, std::milli>(t1 - t0).count());
    }

    std::sort(samples.begin(), samples.end());

    TimingStats stats;
    stats.rows = rows;
    stats.best_ms = samples.front();
    stats.median_ms = samples[samples.size() / 2];
    stats.mean_ms = std::accumulate(samples.begin(), samples.end(), 0.0) / static_cast<double>(samples.size());
    return stats;
}

void PrintTableHeader() {
    std::cout << std::left << std::setw(10) << "mode" << std::right << std::setw(9) << "threads" << std::setw(14)
              << "best(ms)" << std::setw(14) << "median(ms)" << std::setw(14) << "mean(ms)" << std::setw(16) << "rows/s"
              << std::setw(11) << "speedup" << std::setw(14) << "rows" << "\n";
    std::cout << std::string(102, '-') << "\n";
}

void PrintTableRow(const std::string& mode, size_t threads, const TimingStats& stats, double serial_best_ms) {
    const double seconds = stats.best_ms / 1000.0;
    const double rows_per_s = seconds > 0.0 ? static_cast<double>(stats.rows) / seconds : 0.0;
    const double speedup = (serial_best_ms > 0.0 && stats.best_ms > 0.0) ? serial_best_ms / stats.best_ms : 1.0;

    std::cout << std::left << std::setw(10) << mode << std::right << std::setw(9) << threads << std::fixed
              << std::setprecision(2) << std::setw(14) << stats.best_ms << std::setw(14) << stats.median_ms
              << std::setw(14) << stats.mean_ms << std::setw(16) << static_cast<uint64_t>(rows_per_s) << std::setw(10)
              << std::setprecision(2) << speedup << "x" << std::setw(14) << stats.rows << "\n";
}

void RunScenario(const std::string& title, bool with_filter, const Options& opt, const TableSchema& schema,
                 const std::filesystem::path& tables_root) {
    const ScanOptions scan_options = MakeScanOptions(with_filter);

    // 计时区间外只打开一次：串行（cursor 自持进度）与并行都复用该实例。
    auto table = TableStorage::Open(kTableId, schema, tables_root);
    if (table == nullptr) {
        throw std::runtime_error("failed to open table storage");
    }

    std::cout << "\n================ " << title << " ================\n";
    PrintTableHeader();

    const auto serial_fn = [&] { return RunSerialOnce(table.get(), scan_options); };
    const TimingStats serial = TimeIt(serial_fn, opt.warmup, opt.repeats);
    PrintTableRow("serial", 1, serial, serial.best_ms);

    for (size_t threads : opt.threads) {
        const auto parallel_fn = [&] {
            return RunParallelOnce(table.get(), scan_options, threads, opt.queue_capacity);
        };
        const TimingStats stats = TimeIt(parallel_fn, opt.warmup, opt.repeats);
        PrintTableRow("parallel", threads, stats, serial.best_ms);

        if (stats.rows != serial.rows) {
            std::cerr << "  [warn] threads=" << threads << " rows=" << stats.rows << " != serial rows=" << serial.rows
                      << "\n";
        }
    }
}

void PrintUsage(const char* argv0) {
    std::cout << "usage: " << argv0 << " [options]\n"
              << "  --rows N           数据集行数（默认 4000000）\n"
              << "  --repeats R        每个配置计时轮数（默认 5）\n"
              << "  --warmup W         预热轮数（默认 2）\n"
              << "  --batch B          写入侧 DataChunk 行数（默认 8192）\n"
              << "  --queue Q          并行批队列容量（默认 16）\n"
              << "  --threads 1,2,4,8  待测并行线程数（默认 1,2,4,8 截断到硬件并发度）\n"
              << "  --scenario S       scan | filter | all（默认 all）\n"
              << "  --data DIR         数据落盘目录\n"
              << "  --help             显示本帮助\n";
}

std::vector<size_t> ParseThreads(const std::string& text) {
    std::vector<size_t> threads;
    std::stringstream ss(text);
    std::string token;
    while (std::getline(ss, token, ',')) {
        if (!token.empty()) {
            threads.push_back(static_cast<size_t>(std::stoul(token)));
        }
    }
    return threads;
}

std::vector<size_t> DefaultThreads() {
    const size_t hc = std::max<size_t>(1, std::thread::hardware_concurrency());
    std::vector<size_t> threads;
    for (size_t t : {static_cast<size_t>(1), static_cast<size_t>(2), static_cast<size_t>(4), static_cast<size_t>(8)}) {
        if (t <= hc) {
            threads.push_back(t);
        }
    }
    if (threads.empty()) {
        threads.push_back(1);
    }
    if (threads.back() != hc) {
        threads.push_back(hc); // 保证覆盖满核
    }
    return threads;
}

Options ParseArgs(int argc, char** argv) {
    Options opt;

#ifdef SIMPLE_OLAP_BENCH_ROOT_DIR
    opt.data_dir = std::filesystem::path(SIMPLE_OLAP_BENCH_ROOT_DIR) / "benchmark" / "bench_data";
#else
    opt.data_dir = "benchmark/bench_data";
#endif
    opt.threads = DefaultThreads();

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        const auto value = [&]() -> std::string {
            if (i + 1 >= argc) {
                throw std::runtime_error("missing value for " + arg);
            }
            return argv[++i];
        };

        if (arg == "--rows") {
            opt.rows = std::stoull(value());
        } else if (arg == "--repeats") {
            opt.repeats = std::stoi(value());
        } else if (arg == "--warmup") {
            opt.warmup = std::stoi(value());
        } else if (arg == "--batch") {
            opt.batch_rows = static_cast<uint32_t>(std::stoul(value()));
        } else if (arg == "--queue") {
            opt.queue_capacity = static_cast<size_t>(std::stoul(value()));
        } else if (arg == "--threads") {
            opt.threads = ParseThreads(value());
        } else if (arg == "--scenario") {
            opt.scenario = value();
        } else if (arg == "--data") {
            opt.data_dir = value();
        } else if (arg == "--help" || arg == "-h") {
            PrintUsage(argv[0]);
            std::exit(0);
        } else {
            throw std::runtime_error("unknown argument: " + arg);
        }
    }

    if (opt.repeats < 1) {
        opt.repeats = 1;
    }
    if (opt.threads.empty()) {
        opt.threads = DefaultThreads();
    }
    return opt;
}

} // namespace

int main(int argc, char** argv) {
    try {
        const Options opt = ParseArgs(argc, argv);
        const TableSchema schema = MakeSchema();
        const std::filesystem::path tables_root = opt.data_dir / "tables";

        std::cout << "simple_olap parallel-scan benchmark\n"
                  << "  rows         : " << opt.rows << "\n"
                  << "  batch        : " << opt.batch_rows << "\n"
                  << "  repeats      : " << opt.repeats << " (warmup " << opt.warmup << ")\n"
                  << "  queue        : " << opt.queue_capacity << "\n"
                  << "  hardware     : " << std::thread::hardware_concurrency() << " threads\n"
                  << "  data dir     : " << opt.data_dir << "\n";

        std::cout << "\n[build] generating dataset ..." << std::flush;
        const auto build_start = Clock::now();
        auto table = BuildDataset(tables_root, schema, opt.rows, opt.batch_rows);
        const size_t segments = table->segment_count();
        table.reset();
        const double build_ms = std::chrono::duration<double, std::milli>(Clock::now() - build_start).count();
        std::cout << " done in " << std::fixed << std::setprecision(1) << build_ms << " ms, " << segments
                  << " segments\n";

        const bool run_scan = (opt.scenario == "all" || opt.scenario == "scan");
        const bool run_filter = (opt.scenario == "all" || opt.scenario == "filter");
        if (!run_scan && !run_filter) {
            throw std::runtime_error("unknown --scenario: " + opt.scenario);
        }

        if (run_scan) {
            RunScenario("scan-only (no predicate)", /*with_filter=*/false, opt, schema, tables_root);
        }
        if (run_filter) {
            RunScenario("filter (value > 0.5, row-level)", /*with_filter=*/true, opt, schema, tables_root);
        }
    } catch (const std::exception& e) {
        std::cerr << "ERROR: " << e.what() << "\n";
        return 1;
    }

    return 0;
}
