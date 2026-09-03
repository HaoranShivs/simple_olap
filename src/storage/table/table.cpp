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
            metadata.table_id, table_path, metadata);

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

        // 3. 用表元数据（含已落盘 segment id 列表）构造 StorageManager
        auto storage = std::make_unique<StorageManager>(
            table_id, table_path, metadata);

        // 4. 构造 Table
        return std::unique_ptr<Table>(new Table(std::move(metadata), std::move(storage)));
    }

    bool Table::Append(const DataChunk &datachunk)
    {
        // 委托给 StorageManager，由其决定写入活跃 segment 及何时封存
        storage_->Append(datachunk);
        return true;
    }

    uint32_t Table::active_segment_row_count() const noexcept
    {
        return storage_->active_segment_row_count();
    }

    bool Table::Flush()
    {
        // 1. 写回数据：将内存中待刷盘的 segment 写盘
        storage_->Flush();

        // 2. 修改元数据：StorageManager 落盘时已把 segment id 登记进自己的 TableMeta，
        // 这里同步回上层 Table 的元数据
        metadata_.segment_ids = storage_->table_meta().segment_ids;

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

    bool Table::GetVectorBatch(const SelectTargetStatement &request, ScanCursor &cursor, VectorBatch &output)
    {
        // 委托给 StorageManager，由其跨 segment 推进游标并提取数据
        return storage_->GetVectorBatch(request, cursor, output);
    }

} // namespace simple_olap