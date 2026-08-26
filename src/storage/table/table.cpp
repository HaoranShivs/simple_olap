#include "table.h"

#include <fstream>
#include <system_error>

namespace simple_olap
{
    Table::Table(TableMeta metadata, std::unique_ptr<StorageManager> storage)
        : metadata_(std::move(metadata)), storage_(std::move(storage))
    {
    }

    std::unique_ptr<Table> Table::Create(const TableMeta &metadata, const std::filesystem::path &root_dir)
    {
        // 1. 创建表数据目录：root_dir / {table_id}
        const std::filesystem::path table_path = root_dir / std::to_string(metadata.table_id);
        std::error_code ec;
        std::filesystem::create_directories(table_path, ec);
        if (ec)
        {
            return nullptr;
        }

        // 2. 写入表元数据文件：table_path / table.meta
        {
            BinaryWriter writer;
            metadata.Serialize(writer);

            const std::filesystem::path meta_path = table_path / "table.meta";
            std::ofstream file(meta_path, std::ios::binary | std::ios::trunc);
            if (!file)
            {
                return nullptr;
            }

            const auto &buffer = writer.GetBuffer();
            file.write(reinterpret_cast<const char *>(buffer.data()),
                       static_cast<std::streamsize>(buffer.size()));
            if (!file.good())
            {
                return nullptr;
            }
        }

        // 3. 创建 StorageManager（构造时立即创建空 SegmentBuilder）
        auto storage = std::make_unique<StorageManager>(
            metadata.table_id, table_path, metadata.schema);

        // 4. 构造 Table
        return std::unique_ptr<Table>(new Table(metadata, std::move(storage)));
    }

    std::unique_ptr<Table> Table::Open(TableId table_id, const std::filesystem::path &root_dir)
    {
        // 1. 读取并反序列化 table.meta
        const std::filesystem::path table_path = root_dir / std::to_string(table_id);
        const std::filesystem::path meta_path = table_path / "table.meta";

        std::ifstream in(meta_path, std::ios::binary);
        if (!in)
        {
            return nullptr;
        }
        std::vector<uint8_t> buffer((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        in.close();

        BinaryReader reader(buffer);
        TableMeta metadata = TableMeta::Deserialize(reader);

        // 2. 校验 id 一致性
        if (metadata.table_id != table_id)
        {
            return nullptr;
        }

        // 3. 用已封存 segment 的 id 列表构造 StorageManager
        auto storage = std::make_unique<StorageManager>(
            table_id, table_path, metadata.schema, metadata.segment_ids);

        // 4. 构造 Table
        return std::unique_ptr<Table>(new Table(std::move(metadata), std::move(storage)));
    }

    bool Table::Append(const DataChunk &datachunk)
    {
        // 委托给 StorageManager，由其决定写入活跃 segment 及何时封存
        storage_->Append(datachunk);
        return true;
    }

    bool Table::Flush()
    {
        // 1. 写回数据：将内存中待刷盘的 segment 写盘
        storage_->Flush();

        // 2. 修改元数据：把已落盘的 segment 登记进 table_meta
        metadata_.segment_ids = storage_->on_disk_segments();

        // 3. 写回元数据：table_path / table.meta
        BinaryWriter writer;
        metadata_.Serialize(writer);

        const std::filesystem::path meta_path = storage_->path() / "table.meta";
        std::ofstream file(meta_path, std::ios::binary | std::ios::trunc);
        if (!file)
        {
            return false;
        }

        const auto &buffer = writer.GetBuffer();
        file.write(reinterpret_cast<const char *>(buffer.data()),
                   static_cast<std::streamsize>(buffer.size()));

        return file.good();
    }

    const TableSchema &Table::GetSchema() const
    {
        return metadata_.schema;
    }

    std::unique_ptr<class TableScan> Table::Scan(const SelectStatement &options) const
    {
        // 1. 将 name-based SelectStatement 转为 id-based SelectTargetStatement
        SelectTargetStatement target;
        target.table_id = metadata_.table_id;

        // 2. select_list -> target_list（column_name -> column_id）
        target.target_list.reserve(options.select_list.size());
        for (const auto &item : options.select_list)
        {
            // 按列名查找 column_id
            ColumnId col_id = UINT32_MAX;
            for (const auto &col : metadata_.schema.columns)
            {
                if (col.name == item.column_name)
                {
                    col_id = col.column_id;
                    break;
                }
            }
            if (col_id == UINT32_MAX)
            {
                // 列名不存在，返回 nullptr
                return nullptr;
            }
            target.target_list.push_back({col_id, item.op});
        }

        // 3. where 条件转换（column_name -> column_id）
        if (options.where)
        {
            ColumnId col_id = UINT32_MAX;
            for (const auto &col : metadata_.schema.columns)
            {
                if (col.name == options.where->column_name)
                {
                    col_id = col.column_id;
                    break;
                }
            }
            if (col_id == UINT32_MAX)
            {
                return nullptr;
            }
            auto where_target = std::make_unique<ExprTarget>();
            where_target->column_id = col_id;
            where_target->op = options.where->op;
            where_target->value = options.where->value;
            target.where = std::move(where_target);
        }

        // 4. group_by 转换（column_name -> column_id）
        target.group_by.reserve(options.group_by.size());
        for (const auto &expr : options.group_by)
        {
            ColumnId col_id = UINT32_MAX;
            for (const auto &col : metadata_.schema.columns)
            {
                if (col.name == expr->column_name)
                {
                    col_id = col.column_id;
                    break;
                }
            }
            if (col_id == UINT32_MAX)
            {
                return nullptr;
            }
            auto group_target = std::make_unique<ExprTarget>();
            group_target->column_id = col_id;
            group_target->op = expr->op;
            group_target->value = expr->value;
            target.group_by.push_back(std::move(group_target));
        }

        // 5. 委托给 StorageManager::Scan
        return storage_->Scan(target);
    }

} // namespace simple_olap