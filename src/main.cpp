#include <cstdint>
#include <cstring>
#include <exception>
#include <iostream>
#include <memory>
#include <string>
#include <variant>
#include <vector>

#include "execution/scan/scan.h"
#include "execution/vector/vector.h"
#include "planner/binder.h"
#include "sql/ast/expression.h"
#include "sql/ast/statement.h"
#include "sql/lexer/lexer.h"
#include "sql/parser/parser.h"
#include "storage/catalog.h"
#include "storage/datachunk.h"

using namespace simple_olap;

namespace
{
    // ==========================================
    // 辅助：间接统计表行数
    // ==========================================
    // Table 没有直接的行数接口，这里用"扫描第一列"的方式间接统计。
    // 注意：scan 只能看到已 Flush 落盘的 segment，活跃 segment 不可见，
    // 因此调用方在 INSERT 后需要先 Flush 再统计。
    uint64_t CountRows(Catalog &catalog, const std::string &table_name)
    {
        Table *table = catalog.GetTable(table_name);
        if (table == nullptr)
        {
            return 0;
        }
        const TableSchema &schema = table->GetSchema();
        if (schema.columns.empty())
        {
            return 0;
        }

        // 构造 SELECT <第一列> FROM <表> 的全表扫描
        SelectStatement select;
        select.table_name = table_name;
        select.select_list.emplace_back(
            std::make_unique<ColumnRefExpr>(schema.columns[0].name));

        TableScan scan(select, &catalog);
        scan.Init();

        VectorBatch batch;
        uint64_t total_rows = 0;
        while (scan.Next(batch))
        {
            total_rows += batch.size; // 无 where 时 size 即有效行数
        }
        return total_rows;
    }

    // ==========================================
    // SELECT 执行：打印结果行数与列数
    // ==========================================
    void ExecuteSelect(Catalog &catalog, SelectStatement &select)
    {
        // 1. 查表（不存在时报错）
        Table *table = catalog.GetTable(select.table_name);
        if (table == nullptr)
        {
            std::cout << "ERROR: table not found: " << select.table_name << "\n";
            return;
        }
        const TableSchema &schema = table->GetSchema();

        // 2. 展开 SELECT *：替换为全部列（TableScan 只支持简单列引用）
        bool has_star = false;
        for (const auto &item : select.select_list)
        {
            if (item.expr->type == Expr::Type::COLUMN_REF)
            {
                const auto &col = static_cast<const ColumnRefExpr &>(*item.expr);
                if (col.column_name == "*")
                {
                    has_star = true;
                    break;
                }
            }
        }
        if (has_star)
        {
            std::vector<SelectItem> expanded;
            expanded.reserve(schema.columns.size());
            for (const auto &col : schema.columns)
            {
                expanded.emplace_back(std::make_unique<ColumnRefExpr>(col.name));
            }
            select.select_list = std::move(expanded);
        }

        // 3. 语义校验（列是否存在等），失败会抛 SemanticException
        Binder binder(catalog);
        binder.BindStatement(select);

        // 4. 执行扫描，统计行数
        TableScan scan(select, &catalog);
        scan.Init();

        VectorBatch batch;
        uint64_t total_rows = 0;
        while (scan.Next(batch))
        {
            // 有 where 时数据未压缩，有效行由 sel_vector 标记；无 where 时即 batch.size
            const uint64_t valid_rows = select.where_clause
                                            ? static_cast<uint64_t>(batch.sel_vector.size())
                                            : batch.size;
            total_rows += valid_rows;
        }

        std::cout << "SELECT ok: rows=" << total_rows
                  << ", cols=" << select.select_list.size() << "\n";
    }

    // ==========================================
    // INSERT 执行：打印执行前后的数据行数
    // ==========================================
    void ExecuteInsert(Catalog &catalog, const InsertStatement &stmt)
    {
        // 1. 查表
        Table *table = catalog.GetTable(stmt.table_name);
        if (table == nullptr)
        {
            std::cout << "ERROR: table not found: " << stmt.table_name << "\n";
            return;
        }
        const TableSchema &schema = table->GetSchema();

        // 2. 确定目标列：逻辑值下标 -> 物理列下标
        //    未指定列名时按 schema 顺序全列插入；指定列名时按名字映射，
        //    未覆盖的物理列补 0。
        std::vector<int> value_idx_of_col(schema.columns.size(), -1);
        if (stmt.columns.empty())
        {
            for (size_t c = 0; c < schema.columns.size(); ++c)
            {
                value_idx_of_col[c] = static_cast<int>(c);
            }
            if (!stmt.values.empty() &&
                stmt.values[0].size() != schema.columns.size())
            {
                std::cout << "ERROR: VALUES size mismatch with table schema\n";
                return;
            }
        }
        else
        {
            for (size_t v = 0; v < stmt.columns.size(); ++v)
            {
                bool found = false;
                for (size_t c = 0; c < schema.columns.size(); ++c)
                {
                    if (schema.columns[c].name == stmt.columns[v])
                    {
                        value_idx_of_col[c] = static_cast<int>(v);
                        found = true;
                        break;
                    }
                }
                if (!found)
                {
                    std::cout << "ERROR: column not found in table: "
                              << stmt.columns[v] << "\n";
                    return;
                }
            }
            if (!stmt.values.empty() &&
                stmt.values[0].size() != stmt.columns.size())
            {
                std::cout << "ERROR: VALUES size mismatch with column list\n";
                return;
            }
        }

        const size_t nrows = stmt.values.size();
        if (nrows == 0)
        {
            std::cout << "INSERT ok: 0 rows\n";
            return;
        }

        // 3. 执行前的行数（scan 只见已落盘数据）
        const uint64_t rows_before = CountRows(catalog, stmt.table_name);
        const uint32_t active_before = table->active_segment_row_count();

        // 4. 把 LiteralExpr 行转成 DataChunk
        DataChunk chunk(nrows); // capacity 即行数
        for (size_t c = 0; c < schema.columns.size(); ++c)
        {
            const DataType type = schema.columns[c].type;
            const size_t elem_size = TypeElemSize(type);
            auto buf = std::shared_ptr<uint8_t[]>(new uint8_t[nrows * elem_size]());
            const int vidx = value_idx_of_col[c];

            for (size_t r = 0; r < nrows && vidx >= 0; ++r)
            {
                const auto &expr = stmt.values[r][static_cast<size_t>(vidx)];
                if (expr->type != Expr::Type::LITERAL)
                {
                    std::cout << "ERROR: only literal values are supported in INSERT\n";
                    return;
                }
                const auto &literal = static_cast<const LiteralExpr &>(*expr);
                uint8_t *dst = buf.get() + r * elem_size;

                switch (type)
                {
                case DataType::INT32:
                {
                    // 词法层把所有数字都 lex 成 FLOAT token，整数字面量也是 double，
                    // 这里统一转回 int32（非整数值直接截断）
                    if (!std::holds_alternative<double>(literal.value))
                    {
                        std::cout << "ERROR: expected numeric value for column "
                                  << schema.columns[c].name << "\n";
                        return;
                    }
                    const int32_t x = static_cast<int32_t>(std::get<double>(literal.value));
                    std::memcpy(dst, &x, sizeof(x));
                    break;
                }
                case DataType::INT64:
                {
                    // 词法层把所有数字都 lex 成 FLOAT token，整数字面量也是 double，
                    // 这里统一转回 int64（非整数值直接截断）
                    if (!std::holds_alternative<double>(literal.value))
                    {
                        std::cout << "ERROR: expected numeric value for column "
                                  << schema.columns[c].name << "\n";
                        return;
                    }
                    const int64_t x = static_cast<int64_t>(std::get<double>(literal.value));
                    std::memcpy(dst, &x, sizeof(x));
                    break;
                }
                case DataType::FLOAT:
                {
                    if (!std::holds_alternative<double>(literal.value))
                    {
                        std::cout << "ERROR: expected numeric value for column "
                                  << schema.columns[c].name << "\n";
                        return;
                    }
                    const float x = static_cast<float>(std::get<double>(literal.value));
                    std::memcpy(dst, &x, sizeof(x));
                    break;
                }
                case DataType::DOUBLE:
                {
                    if (!std::holds_alternative<double>(literal.value))
                    {
                        std::cout << "ERROR: expected numeric value for column "
                                  << schema.columns[c].name << "\n";
                        return;
                    }
                    const double x = std::get<double>(literal.value);
                    std::memcpy(dst, &x, sizeof(x));
                    break;
                }
                default:
                    std::cout << "ERROR: unsupported column type for INSERT: "
                              << schema.columns[c].name << "\n";
                    return;
                }
            }
            chunk.set_data(c, std::move(buf), elem_size);
        }

        // 5. 写入并落盘（scan 只能看到已 Flush 的 segment，必须 Flush 后统计才准确）
        if (!table->Append(chunk))
        {
            std::cout << "ERROR: Append failed\n";
            return;
        }
        if (!table->Flush())
        {
            std::cout << "ERROR: Flush failed\n";
            return;
        }

        // 6. 执行后的行数
        const uint64_t rows_after = CountRows(catalog, stmt.table_name);
        const uint32_t active_after = table->active_segment_row_count();
        std::cout << "INSERT ok: rows before=" << rows_before
                  << ", rows after=" << rows_after
                  << " (+" << nrows << ")\n";
        // 临时观察接口：展示 Insert 前后活跃 segment 的行数变化
        std::cout << "  active_segment rows: before=" << active_before
                  << ", after=" << active_after << "\n";
    }

    // ==========================================
    // CREATE TABLE 执行：打印执行前后的表名列表
    // ==========================================
    void PrintTableNames(Catalog &catalog)
    {
        const auto &name_id = catalog.GetCatalogMeta().table_name_id;
        std::cout << "  tables(" << name_id.size() << "):";
        if (name_id.empty())
        {
            std::cout << " (none)";
        }
        for (const auto &kv : name_id)
        {
            std::cout << " " << kv.first;
        }
        std::cout << "\n";
    }

    void ExecuteCreateTable(Catalog &catalog, const CreateTableStatement &stmt)
    {
        std::cout << "CREATE TABLE before:\n";
        PrintTableNames(catalog);

        if (!catalog.CreateTable(stmt))
        {
            std::cout << "ERROR: CreateTable failed: " << stmt.table_name << "\n";
            return;
        }

        std::cout << "CREATE TABLE after:\n";
        PrintTableNames(catalog);
    }

    // ==========================================
    // 单条 SQL 的完整流水线：Lexer -> Parser -> 分发执行
    // ==========================================
    void ExecuteSql(Catalog &catalog, const std::string &sql)
    {
        // 1. 词法分析
        Lexer lexer(sql);
        std::vector<Token> tokens = lexer.Tokenize();
        if (tokens.empty() || tokens.front().type == TokenType::END)
        {
            return; // 空语句
        }

        // 2. 语法分析
        Parser parser(std::move(tokens));
        StatementPtr stmt = parser.ParseStatement();

        // 3. 按语句类型分发
        switch (stmt->GetType())
        {
        case Statement::Type::SELECT:
            ExecuteSelect(catalog, static_cast<SelectStatement &>(*stmt));
            break;
        case Statement::Type::INSERT:
            ExecuteInsert(catalog, static_cast<const InsertStatement &>(*stmt));
            break;
        case Statement::Type::CREATE_TABLE:
            ExecuteCreateTable(catalog, static_cast<const CreateTableStatement &>(*stmt));
            break;
        default:
            std::cout << "ERROR: unsupported statement type\n";
            break;
        }
    }

    void PrintHelp()
    {
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

int main()
{
    // 根目录：使用 CMake 注入的项目根目录（SIMPLE_OLAP_ROOT_DIR），
    // 保证无论从哪个工作目录运行，数据都落在项目内的 database/ 下
    std::filesystem::path database_path = SIMPLE_OLAP_ROOT_DIR "/database";

    // 载入 catalog 元数据；不存在则新建
    Catalog catalog;
    if (!catalog.LoadMeta(database_path))
    {
        catalog.Create(database_path);
    }

    PrintHelp();

    // REPL 主循环：多行输入，遇到 ';' 才执行
    std::string buffer;
    std::string line;
    std::cout << "sql> " << std::flush;
    while (std::getline(std::cin, line))
    {
        // 去掉行首尾空白后判断退出命令
        const size_t first = line.find_first_not_of(" \t\r\n");
        if (first != std::string::npos)
        {
            const size_t last = line.find_last_not_of(" \t\r\n");
            const std::string trimmed = line.substr(first, last - first + 1);
            if (buffer.empty() && (trimmed == "exit" || trimmed == "quit"))
            {
                break;
            }
        }

        buffer += line + "\n";

        // 遇到分号则执行缓冲区中的语句
        if (buffer.find(';') != std::string::npos)
        {
            // 分号交给词法层处理（SEMICOLON token），直接整段送入流水线
            try
            {
                ExecuteSql(catalog, buffer);
            }
            catch (const std::exception &e)
            {
                std::cout << "ERROR: " << e.what() << "\n";
            }
            buffer.clear();
        }

        std::cout << (buffer.empty() ? "sql> " : "  -> ") << std::flush;
    }

    std::cout << "\nbye\n";
    return 0;
}
