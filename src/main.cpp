#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <exception>
#include <filesystem>
#include <iostream>
#include <memory>
#include <random>
#include <sstream>
#include <string>
#include <variant>
#include <vector>

#include "catalog/table_catalog_entry.h"
#include "main/connection.h"
#include "main/database.h"
#include "main/query_result.h"
#include "sql/ast/statement.h"
#include "storage/datachunk.h"
#include "storage/datastructs.h"
#include "storage/table/table_storage.h"
#include "type.h"

#ifndef SIMPLE_OLAP_ROOT_DIR
#define SIMPLE_OLAP_ROOT_DIR "."
#endif

using namespace simple_olap;

namespace {

// 基准测试用：--silent 时跳过结果集的逐行格式化输出，只保留行数统计，
// 避免结果打印（I/O）主导执行计时。
bool g_silent = false;

// ==========================================
// ReadSql：从 stdin 读取一条完整 SQL 语句
// ==========================================
// 规则：
//   - 语句以 ';' 结尾（';' 本身不进入 sql）
//   - 支持跨多行输入（未遇到 ';' 前继续读下一行）
//   - "exit" / "quit"（单独一行）返回 false 退出 REPL
//   - EOF 返回 false
bool ReadSql(std::string& sql) {
    sql.clear();

    std::cout << "simple_olap> " << std::flush;

    std::string line;
    while (std::getline(std::cin, line)) {
        // 去掉行尾 \r（Windows 换行）
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) {
            line.pop_back();
        }

        // 单独一行 exit / quit：退出 REPL
        if (sql.empty() && (line == "exit" || line == "quit")) {
            return false;
        }

        // 查找语句结束符 ';'
        const size_t semi = line.find(';');
        if (semi == std::string::npos) {
            // 未结束：累积并继续读下一行
            if (!line.empty()) {
                sql += line;
                sql += ' ';
            }
            std::cout << "        -> " << std::flush;
            continue;
        }

        // 结束：截取 ';' 之前的部分
        sql += line.substr(0, semi);

        // 去除首尾空白后仍为空则视为空语句，继续读下一条
        const size_t first = sql.find_first_not_of(" \t");
        if (first == std::string::npos) {
            sql.clear();
            std::cout << "simple_olap> " << std::flush;
            continue;
        }

        return true;
    }

    // EOF
    std::cout << "\n";
    return false;
}

// ==========================================
// PrintResult：按语句类型打印执行结果
// ==========================================

// 把一个 batch 的有效行打印成表格（列间以 " | " 分隔）
void PrintBatch(const QueryResult& result, const VectorBatch& batch) {
    const uint32_t row_count = batch.sel_vector.empty() ? batch.size : static_cast<uint32_t>(batch.sel_vector.size());
    if (row_count == 0) {
        return;
    }

    for (uint32_t logical_row = 0; logical_row < row_count; ++logical_row) {
        const uint32_t physical_row = batch.sel_vector.empty() ? logical_row : batch.sel_vector[logical_row];

        std::cout << "  ";
        for (uint32_t c = 0; c < batch.columns.size(); ++c) {
            const ColumnData& col = batch.columns[c];

            if (c > 0) {
                std::cout << " | ";
            }

            if (col.buffer == nullptr || physical_row >= col.count) {
                std::cout << "NULL";
                continue;
            }

            const uint8_t* ptr = col.buffer + static_cast<size_t>(physical_row) * TypeElemSize(col.type);
            switch (col.type) {
            case DataType::INT32:
                std::cout << *reinterpret_cast<const int32_t*>(ptr);
                break;
            case DataType::INT64:
                std::cout << *reinterpret_cast<const int64_t*>(ptr);
                break;
            case DataType::FLOAT:
                std::cout << *reinterpret_cast<const float*>(ptr);
                break;
            case DataType::DOUBLE:
                std::cout << *reinterpret_cast<const double*>(ptr);
                break;
            case DataType::VARCHAR:
                std::cout << ReadVarcharSlot(ptr);
                break;
            default:
                std::cout << "?";
                break;
            }
        }
        std::cout << "\n";
    }
}

void PrintResult(const QueryResult& result) {
    switch (result.type) {
    case QueryResult::Type::SELECT: {
        uint64_t total_rows = 0;
        for (const auto& batch : result.chunks) {
            total_rows += batch.size;
        }

        // 基准测试：只输出行数，跳过表头与逐行格式化
        if (g_silent) {
            std::cout << "(" << total_rows << " rows)\n";
            break;
        }

        // 表头
        std::cout << "  ";
        for (size_t c = 0; c < result.columns.size(); ++c) {
            if (c > 0) {
                std::cout << " | ";
            }
            std::cout << result.columns[c].name;
        }
        std::cout << "\n";

        // 数据行
        for (const auto& batch : result.chunks) {
            PrintBatch(result, batch);
        }

        std::cout << "(" << total_rows << " rows)\n";
        break;
    }

    case QueryResult::Type::INSERT:
        std::cout << "INSERT ok: " << result.affected_rows << " rows\n";
        break;

    case QueryResult::Type::CREATE_TABLE:
        std::cout << "CREATE TABLE ok\n";
        break;
    }
}

const char* ModeName(ExecutionMode mode) {
    switch (mode) {
    case ExecutionMode::AUTO:
        return "auto";
    case ExecutionMode::SINGLE_THREAD:
        return "single";
    case ExecutionMode::MULTI_THREAD:
        return "multi";
    }
    return "unknown";
}

// 运行时元命令：SET MODE auto|single|multi;
// 在 SQL 解析之前拦截，直接切换后续查询的执行模式。
bool TryHandleSetMode(const std::string& sql, Database& database) {
    std::string lowered;
    lowered.reserve(sql.size());
    for (unsigned char c : sql) {
        lowered.push_back(static_cast<char>(std::tolower(c)));
    }

    std::istringstream iss(lowered);
    std::string first;
    std::string second;
    std::string third;
    if (!(iss >> first >> second >> third)) {
        return false;
    }
    if (first != "set" || second != "mode") {
        return false;
    }

    ExecutionMode mode;
    if (third == "auto") {
        mode = ExecutionMode::AUTO;
    } else if (third == "single" || third == "serial") {
        mode = ExecutionMode::SINGLE_THREAD;
    } else if (third == "multi" || third == "parallel") {
        mode = ExecutionMode::MULTI_THREAD;
    } else {
        std::cout << "ERROR: unknown mode '" << third << "' (auto|single|multi)\n";
        return true;
    }

    database.SetExecutionMode(mode);
    std::cout << "execution mode = " << ModeName(mode) << "\n";
    return true;
}

// ==========================================
// GenerateDataset：基准测试用数据集生成
// ==========================================
// 生成 (id INT64, value DOUBLE) 两列表，共 rows 行：
//   - 每 batch_rows 行组装成一个 DataChunk 追加（写入侧批大小）
//   - 固定随机种子，与 bench_parallel_scan::BuildDataset 结果一致、可复现
//   - 最后一次 Flush 把活动 segment 落盘，数据才对后续扫描可见
// 返回进程退出码，生成完成后调用方直接退出（不进入 REPL）。
int GenerateDataset(Database& database, const std::string& table_name, uint64_t rows, uint32_t batch_rows) {
    if (batch_rows == 0) {
        std::cerr << "--batch must be > 0\n";
        return 1;
    }

    // 已存在同名表：先删除，保证可反复生成同一张基准表
    if (database.GetCatalog().FindTable(table_name).has_value()) {
        if (!database.DropTable(table_name)) {
            std::cerr << "failed to drop existing table: " << table_name << "\n";
            return 1;
        }
    }

    CreateTableStatement stmt;
    stmt.table_name = table_name;
    stmt.columns.push_back(ColumnSchema{0, "id", DataType::INT64});
    stmt.columns.push_back(ColumnSchema{1, "value", DataType::DOUBLE});

    if (!database.CreateTable(stmt)) {
        std::cerr << "failed to create table: " << table_name << "\n";
        return 1;
    }

    const auto table_id = database.GetCatalog().FindTable(table_name);
    if (!table_id.has_value()) {
        std::cerr << "table not found in catalog after create: " << table_name << "\n";
        return 1;
    }
    const TableCatalogEntry* entry = database.GetCatalog().GetTable(*table_id);
    if (entry == nullptr) {
        std::cerr << "catalog entry missing for table: " << table_name << "\n";
        return 1;
    }

    auto storage = database.GetStorageManager().GetTable(*table_id, entry->schema);
    if (storage == nullptr) {
        std::cerr << "failed to open storage for table: " << table_name << "\n";
        return 1;
    }

    // 与 bench_parallel_scan 相同的种子与分布：value ~ U(0, 1)
    std::mt19937_64 rng(20240910ULL);
    std::uniform_real_distribution<double> dist(0.0, 1.0);

    uint64_t written = 0;
    while (written < rows) {
        const uint32_t n = static_cast<uint32_t>(std::min<uint64_t>(batch_rows, rows - written));

        std::shared_ptr<uint8_t[]> id_buffer(new uint8_t[static_cast<size_t>(n) * sizeof(int64_t)]);
        std::shared_ptr<uint8_t[]> value_buffer(new uint8_t[static_cast<size_t>(n) * sizeof(double)]);

        auto* ids = reinterpret_cast<int64_t*>(id_buffer.get());
        auto* values = reinterpret_cast<double*>(value_buffer.get());
        for (uint32_t i = 0; i < n; ++i) {
            ids[i] = static_cast<int64_t>(written + i);
            values[i] = dist(rng);
        }

        DataChunk chunk(n); // capacity 即行数
        chunk.set_data(0, id_buffer, sizeof(int64_t));
        chunk.set_data(1, value_buffer, sizeof(double));

        storage->Append(chunk);
        written += n;
    }

    if (!storage->Flush()) {
        std::cerr << "failed to flush table: " << table_name << "\n";
        return 1;
    }

    std::cout << "generated " << rows << " rows into table '" << table_name << "' (batch=" << batch_rows << ")\n";
    return 0;
}

void PrintHelp() {
    std::cout << "simple_olap SQL REPL\n"
              << "Supported statements (end with ';'):\n"
              << "  CREATE TABLE t (col INT, col2 DOUBLE, ...);\n"
              << "  INSERT INTO t VALUES (1, 3.5), (2, 4.5);\n"
              << "  INSERT INTO t (a, b) VALUES (1, 3.5);\n"
              << "  SELECT * FROM t;\n"
              << "  SELECT a, b FROM t WHERE a > 10;\n"
              << "  SET MODE auto|single|multi;  -- 切换单线程/多线程执行\n"
              << "  exit | quit    -- leave the REPL\n"
              << "CLI options:\n"
              << "  --mode auto|single|multi   initial execution mode\n"
              << "  --threads N                compute/scan worker count\n"
              << "  --scan-threads N           storage scan worker count\n"
              << "  --compute-threads N        compute worker count\n"
              << "  --queue N                  parallel batch queue capacity\n"
              << "  --db DIR                   override database root directory\n"
              << "  --gen-table NAME           generate (id INT64, value DOUBLE) table\n"
              << "  --rows N                   rows to generate (default 4000000)\n"
              << "  --batch B                  DataChunk rows per write (default 8192)\n"
              << "  --drop-table NAME          drop a table and exit\n"
              << "  --silent                   skip row output (benchmark)\n";
}

} // namespace

int main(int argc, char** argv) {
    DatabaseConfig config;

    // 根目录：默认使用 CMake 注入的项目根目录（SIMPLE_OLAP_ROOT_DIR），
    // 保证无论从哪个工作目录运行，数据都落在项目内的 database/ 下；
    // --db 可覆盖，便于基准测试使用独立数据库目录、不污染主库。
    std::filesystem::path database_path = SIMPLE_OLAP_ROOT_DIR "/database";

    // 数据集生成 / 删除相关参数（与 benchmark/bench_parallel_scan.cpp 对齐）
    std::string gen_table;
    std::string drop_table;
    uint64_t gen_rows = 4'000'000;
    uint32_t gen_batch = 8192;

    // 命令行：--mode auto|single|multi，--threads N
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--mode" && i + 1 < argc) {
            const std::string value = argv[++i];
            if (value == "auto") {
                config.execution_mode = ExecutionMode::AUTO;
            } else if (value == "single" || value == "serial") {
                config.execution_mode = ExecutionMode::SINGLE_THREAD;
            } else if (value == "multi" || value == "parallel") {
                config.execution_mode = ExecutionMode::MULTI_THREAD;
            } else {
                std::cerr << "unknown --mode: " << value << " (auto|single|multi)\n";
                return 1;
            }
        } else if (arg == "--threads" && i + 1 < argc) {
            config.thread_count = static_cast<size_t>(std::stoul(argv[++i]));
            // --threads 同时决定并行 compute / scan 的 worker 数
            config.parallel_config.compute_threads = config.thread_count;
            config.parallel_config.scan_threads = config.thread_count;
        } else if (arg == "--compute-threads" && i + 1 < argc) {
            config.parallel_config.compute_threads = static_cast<size_t>(std::stoul(argv[++i]));
        } else if (arg == "--scan-threads" && i + 1 < argc) {
            config.parallel_config.scan_threads = static_cast<size_t>(std::stoul(argv[++i]));
        } else if (arg == "--queue" && i + 1 < argc) {
            config.parallel_config.batch_queue_capacity = static_cast<size_t>(std::stoul(argv[++i]));
        } else if ((arg == "--db" || arg == "--database") && i + 1 < argc) {
            database_path = argv[++i];
        } else if (arg == "--gen-table" && i + 1 < argc) {
            gen_table = argv[++i];
        } else if (arg == "--drop-table" && i + 1 < argc) {
            drop_table = argv[++i];
        } else if (arg == "--rows" && i + 1 < argc) {
            gen_rows = std::stoull(argv[++i]);
        } else if (arg == "--batch" && i + 1 < argc) {
            gen_batch = static_cast<uint32_t>(std::stoul(argv[++i]));
        } else if (arg == "--silent" || arg == "--quiet" || arg == "-q") {
            g_silent = true;
        } else if (arg == "--help" || arg == "-h") {
            PrintHelp();
            return 0;
        } else {
            std::cerr << "unknown argument: " << arg << "\n";
            return 1;
        }
    }

    Database database(database_path, config);

    // 一次性动作：删除表 / 生成数据集，完成后立即退出，不进入 REPL
    if (!drop_table.empty()) {
        if (!database.DropTable(drop_table)) {
            std::cerr << "failed to drop table: " << drop_table << "\n";
            return 1;
        }
        std::cout << "dropped table '" << drop_table << "'\n";
        return 0;
    }

    if (!gen_table.empty()) {
        return GenerateDataset(database, gen_table, gen_rows, gen_batch);
    }

    Connection connection(database);

    PrintHelp();
    std::cout << "execution mode: " << ModeName(config.execution_mode) << "\n";

    std::string sql;

    while (ReadSql(sql)) {
        // 元命令：SET MODE ...; 直接切换执行模式，不进入 SQL 解析
        if (TryHandleSetMode(sql, database)) {
            continue;
        }

        try {
            QueryResult result = connection.Query(sql);

            PrintResult(result);
        } catch (const std::exception& e) {
            std::cout << "ERROR: " << e.what() << "\n";
        }
    }

    return 0;
}
