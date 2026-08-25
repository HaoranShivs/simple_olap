#include "catalog.h"

#include <fstream>
#include <system_error>
#include <unordered_set>

namespace simple_olap
{
    // 元数据文件约定：位于 catalog 根目录下的 catalog.meta
    // Create / LoadMeta / SaveMeta 的 path 参数均指 catalog 根目录。

    bool Catalog::Create(const std::filesystem::path &path)
    {
        // 1. 创建 catalog 根目录（含父目录）
        std::error_code ec;
        std::filesystem::create_directories(path, ec);
        if (ec)
        {
            return false;
        }

        // 2. 若元数据文件已存在，说明该 catalog 已创建过，拒绝覆盖
        const std::filesystem::path meta_path = path / "catalog.meta";
        if (std::filesystem::exists(meta_path, ec))
        {
            return false;
        }

        // 3. 初始化空元数据并写入磁盘
        metadata_ = CatalogMeta{};
        root_path_ = path;

        BinaryWriter writer;
        metadata_.Serialize(writer);

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

    bool Catalog::LoadMeta(const std::filesystem::path &path)
    {
        // 1. 元数据文件必须存在
        const std::filesystem::path meta_path = path / "catalog.meta";
        std::error_code ec;
        if (!std::filesystem::exists(meta_path, ec) || ec)
        {
            return false;
        }

        // 2. 读入整个文件
        std::ifstream file(meta_path, std::ios::binary);
        if (!file)
        {
            return false;
        }

        std::vector<uint8_t> buffer(
            (std::istreambuf_iterator<char>(file)),
            std::istreambuf_iterator<char>());
        if (!file.good() && !file.eof())
        {
            return false;
        }

        // 3. 反序列化到内存元数据
        try
        {
            BinaryReader reader(buffer);
            metadata_ = CatalogMeta::Deserialize(reader);
        }
        catch (const std::exception &)
        {
            // 文件损坏或格式不匹配
            return false;
        }

        root_path_ = path;
        return true;
    }

    bool Catalog::SaveMeta(const std::filesystem::path &path) const
    {
        // 1. 序列化元数据
        BinaryWriter writer;
        metadata_.Serialize(writer);

        // 2. 写入磁盘（覆盖已有文件）
        const std::filesystem::path meta_path = path / "catalog.meta";
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

    bool Catalog::CreateTable(const CreateTableStatement &stmt)
    {
        const std::string &table_name = stmt.table_name;
        const auto &columns = stmt.columns;

        // 1. 重名检查
        if (metadata_.table_name_id.count(table_name) > 0)
        {
            return false;
        }

        // 2. 分配 table_id：现有最大 id + 1（空 catalog 从 1 开始）
        uint32_t next_table_id = 0;
        for (const auto &kv : metadata_.table_name_id)
        {
            if (kv.second >= next_table_id)
            {
                next_table_id = kv.second + 1;
            }
        }

        // 3. 检查列名重复
        std::unordered_set<std::string> seen_column_names;
        for (const auto &col : columns)
        {
            if (seen_column_names.count(col.name) > 0)
            {
                return false;
            }
            seen_column_names.insert(col.name);
        }

        // 4. 转换 columns -> TableSchema（column_id 按顺序从 0 分配）
        TableSchema schema;
        schema.columns.reserve(columns.size());
        for (uint32_t i = 0; i < static_cast<uint32_t>(columns.size()); ++i)
        {
            schema.columns.push_back(ColumnSchema{
                i,
                columns[i].name,
                columns[i].type
            });
        }

        // 5. 更新内存元数据
        metadata_.table_name_id[table_name] = next_table_id;

        // 6. 立即持久化元数据
        if (!SaveMeta(root_path_))
        {
            // 持久化失败，回滚内存元数据
            metadata_.table_name_id.erase(table_name);
            return false;
        }

        // 7. 调用 Table::Create 创建表数据目录（root_path_ / tables / {table_id}）
        TableMeta table_meta;
        table_meta.table_id = next_table_id;
        table_meta.name = table_name;
        table_meta.schema = schema;

        auto table = Table::Create(table_meta, root_path_ / "tables");
        if (!table)
        {
            // 创建失败，回滚内存元数据并重新持久化
            metadata_.table_name_id.erase(table_name);
            SaveMeta(root_path_);
            return false;
        }

        tables_ptr[next_table_id] = std::move(table);
        return true;
    }

    std::optional<TableId> Catalog::FindTable(std::string_view table_name) const
    {
        // 仅查元数据，不要求表已载入内存
        const auto it = metadata_.table_name_id.find(std::string(table_name));
        if (it == metadata_.table_name_id.end())
        {
            return std::nullopt;
        }
        return it->second;
    }

    bool Catalog::LoadTable(const std::string &table_name)
    {
        // 1. 查元数据获取 table_id
        std::optional<TableId> id = FindTable(table_name);
        if (!id.has_value())
        {
            return false;
        }

        // 2. 已载入内存则直接返回
        if (tables_ptr.count(*id) > 0)
        {
            return true;
        }

        // 3. 从硬盘打开表（root_path_ / tables / {table_id}）
        auto table = Table::Open(*id, root_path_ / "tables");
        if (!table)
        {
            return false;
        }

        // 4. 存入内存
        tables_ptr[*id] = std::move(table);
        return true;
    }

} // namespace simple_olap