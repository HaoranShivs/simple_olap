#include <cstdint>
#include <cstring>
#include <exception>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>
#include <variant>
#include <vector>

#include "main/connection.h"
#include "main/database.h"
#include "main/query_result.h"
#include "type.h"

#ifndef SIMPLE_OLAP_ROOT_DIR
#define SIMPLE_OLAP_ROOT_DIR "."
#endif

using namespace simple_olap;

namespace {

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
        uint64_t total_rows = 0;
        for (const auto& batch : result.chunks) {
            PrintBatch(result, batch);
            total_rows += batch.size;
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

void PrintHelp() {
    std::cout << "simple_olap SQL REPL\n"
              << "Supported statements (end with ';'):\n"
              << "  CREATE TABLE t (col INT, col2 DOUBLE, ...);\n"
              << "  INSERT INTO t VALUES (1, 3.5), (2, 4.5);\n"
              << "  INSERT INTO t (a, b) VALUES (1, 3.5);\n"
              << "  SELECT * FROM t;\n"
              << "  SELECT a, b FROM t WHERE a > 10;\n"
              << "  exit | quit    -- leave the REPL\n";
}

} // namespace

int main() {
    // 根目录：使用 CMake 注入的项目根目录（SIMPLE_OLAP_ROOT_DIR），
    // 保证无论从哪个工作目录运行，数据都落在项目内的 database/ 下
    std::filesystem::path database_path = SIMPLE_OLAP_ROOT_DIR "/database";

    Database database(database_path);
    Connection connection(database);

    PrintHelp();

    std::string sql;

    while (ReadSql(sql)) {
        try {
            QueryResult result = connection.Query(sql);

            PrintResult(result);
        } catch (const std::exception& e) {
            std::cout << "ERROR: " << e.what() << "\n";
        }
    }

    return 0;
}
