#include <cstring>
#include <iostream>
#include <memory>
#include <random>
#include <string>
#include <variant>
#include <vector>

#include "execution/scan/scan.h"
#include "storage/catalog.h"
#include "storage/datachunk.h"

using namespace simple_olap;

namespace
{
    // 测试表名与 schema（绕过 SQL，直接在代码中预设指令）
    const std::string kTableName = "test_table";

    CreateTableStatement MakeCreateTable()
    {
        CreateTableStatement stmt;
        stmt.table_name = kTableName;
        stmt.columns = {
            {0, "user_id", DataType::INT32},
            {1, "age", DataType::INT64},
            {2, "score", DataType::DOUBLE},
        };
        return stmt;
    }

    // 按表 schema 生成一条 INSERT 指令：nrows 行随机数据
    // （正式 AST 的 InsertStatement.values 是 ExprPtr，此处用 LiteralExpr 承载随机值）
    InsertStatement MakeInsert(const TableSchema &schema, size_t nrows, std::mt19937 &rng)
    {
        InsertStatement stmt;
        stmt.table_name = kTableName;
        stmt.values.reserve(nrows);

        std::uniform_int_distribution<int32_t> user_dist(0, 1000000);
        std::uniform_int_distribution<int32_t> 
        age_dist(18, 80);
        std::uniform_real_distribution<double> score_dist(0.0, 100.0);

        for (size_t r = 0; r < nrows; ++r)
        {
            std::vector<ExprPtr> row;
            row.reserve(schema.columns.size());
            for (const auto & 
            col : schema.columns)
            {
                switch (col.type)
                {
                case DataType::INT32:
                    row.push_back(std::make_unique<LiteralExpr>(user_dist(rng)));
                    break;
                case DataType::INT64:
                    // LiteralExpr 暂只支持 int32_t，INT64 值先以 int32 承载
                    row.push_back(std::make_unique<LiteralExpr>(age_dist(rng)));
                    break;
                case DataType::DOUBLE:
                    row.push_back(std::make_unique<LiteralExpr>(score_dist(rng)));
                    break;
                default:
                    row.push_back(std::make_unique<LiteralExpr>(std::string("n/a")));
                    break;
                }
            }
            stmt.values.push_back(std::move(row));
        }
        return stmt;
    }

    // 把 InsertStatement（按列顺序的 LiteralExpr 行）转成表存储需要的 DataChunk
    DataChunk ToDataChunk(const InsertStatement &stmt, const TableSchema &schema)
    {
        const size_t nrows = stmt.values.size();
        const size_t ncols = schema.columns.size();

        DataChunk chunk(nrows); // capacity 即行数

        for (size_t c = 0; c < ncols; ++c)
        {
            const DataType type = schema.columns[c].type;
            const size_t elem_size = TypeElemSize(type);
            auto buf = std::shared_ptr<uint8_t[]>(new uint8_t[nrows * elem_size]);

            for (size_t r = 0; r < nrows; ++r)
            {
                const auto &expr = stmt.values[r][c];
                const auto &literal = static_cast<const LiteralExpr &>(*expr);
                uint8_t *dst = buf.get() + r * elem_size;
                switch (type)
                {
                case DataType::INT32:
                {
                    const int32_t x = std::get<int32_t>(literal.value);
                    std::memcpy(dst, &x, sizeof(x));
                    break;
                }
                case DataType::INT64:
                {
                    // LiteralExpr 暂只支持 int32_t，写入时扩展为 int64
                    const int64_t x = std::get<int32_t>(literal.value);
                    std::memcpy(dst, &x, sizeof(x));
                    break;
                }
                case DataType::DOUBLE:
                {
                    const double x = std::get<double>(literal.value);
                    std::memcpy(dst, 
                    &x, sizeof(x));
                    break;
                }
                default:
                    break;
                }
            }
            chunk.set_data(c, std::move(buf), elem_size);
        }
        return chunk;
    }

    // 测试 scan：跑完整个 pull 循环，输出每批的行数与列数
    // where 条件用正式 AST 的 BinaryOpExpr(column > literal) 表示
    void RunScan(Catalog &catalog, const std::string &label, ExprPtr where)
    {
        SelectStatement select;
        select.table_name = kTableName;
        select.select_list.emplace_back(
            std::make_unique<ColumnRefExpr>("user_id"));
        select.select_list.emplace_back(
            std::make_unique<ColumnRefExpr>("age"));
        select.select_list.emplace_back(
            std::make_unique<ColumnRefExpr>("score"));
        select.where_clause = std::move(where);
        // group_by 为空，使用默认构造，避免对 unique_ptr 的拷贝赋值

        TableScan scan(select, &catalog);
        scan.Init();

        VectorBatch batch;
        uint64_t total_rows = 0;
        uint64_t total_batches = 0;
        while (scan.Next(batch))
        {
            ++total_batches;
            // 有 where 时数据未压缩，有效行由 sel_vector 标记；无 where 时即 batch.size
            const uint64_t 
            valid_rows =
                select.where_clause ? static_cast<uint64_t>(batch.sel_vector.size()) : batch.size;
            total_rows += valid_rows;
            std::cout << "  batch #" << 
            total_batches
                      << ": rows=" << valid_rows
                      << ", cols=" << batch.ColumnCount() << "\n";
        }
        std::cout << "[" << label << "] batches=" << total_batches
                  << ", total_rows=" << total_rows << "\n";
    }
} // namespace

int main()
{
    // 根目录：使用 CMake 注入的项目根目录（SIMPLE_OLAP_ROOT_DIR），
    // 保证无论从哪个工作目录运行，数据都落在项目内的 database/ 下
    std::filesystem::path database_path = SIMPLE_OLAP_ROOT_DIR "/database";

    // 首先查看catalog是否能载入
    Catalog catalog;
    if (!catalog.LoadMeta(database_path))
    {
        catalog.Create(database_path);
    }

    // 首先查看表有哪些，如果有，就打开，没有的话，就创建
    const TableSchema *schema = nullptr;
    if (!catalog.FindTable(kTableName).has_value())
    {
        std::cout << "table not found, creating: " << kTableName << "\n";
        if (!catalog.CreateTable(MakeCreateTable()))
        {
            std::cerr << "CreateTable failed\n";
            return 1;
        }
    }
    else
    {
        std::cout << "table exists, opening: " << kTableName << "\n";
    }

    Table *table = catalog.GetTable(kTableName);
    if (table == nullptr)
    {
        std::cerr << "GetTable failed\n";
        return 1;
    }
    schema = &table->GetSchema();

    // 再打开表，首先测试添加数据的功能。注意，这里先绕过数据库命令，直接在代码预设指令。
    // 3 条插入指令，数据随机。
    // 每条取 kMaxSegmentRowCount（65536）行：3 条共 196608 行，
    // 触发 2 次 segment 封存（2 段 x 65536 = 131072 行已封存）+ 1 个活跃 segment（65536 行，仍在内存）。
    // StorageManager::Flush 只落盘已封存的 segment，故落盘 2 段、可扫描 131072 行；
    // 活跃 segment 未落盘，本测试不扫描它。多段数据用于验证跨 segment 游标推进。
    std::mt19937 rng(42);
    const size_t rows_per_stmt = kMaxSegmentRowCount; // 65536
    for (int i = 0; i < 3; ++i)
    {
        InsertStatement ins = MakeInsert(*schema, rows_per_stmt, rng);
        DataChunk chunk = ToDataChunk(ins, *schema);
        if (!table->Append(chunk))
        {
            std::cerr << "Append failed at insert #" << i << "\n";
            return 1;
        }
        std::cout << "insert #" << i << ": appended " << chunk.size() << " rows\n";
    }

    // 落盘：封存的 segment 写入磁盘并登记进 TableMeta，scan 才能读到
    if (!table->Flush())
    {
        std::cerr << "Flush failed\n";
        return 1;
    }
    std::cout << "flush done\n";

    // 插入完成后，测试scan功能：暂时用print()输出有几行，有几列
    // 1. 全表扫描（无 where）
    RunScan(catalog, "full scan", nullptr);

    // 2. 带 where 的扫描：user_id > 500000，
    //    同时验证行级过滤（sel_vector）与跨 segment 游标推进
    ExprPtr cond = std::make_unique<BinaryOpExpr>(
        BinaryOpExpr::OpType::GT,
        std::make_unique<ColumnRefExpr>("user_id"),
        std::make_unique<LiteralExpr>(500000));
    RunScan(catalog, "scan where user_id > 500000", std::move(cond));

    return 0;
}
