#pragma once

#include <memory>
#include <optional>
#include <string_view>
#include "datastructs.h"
#include "test_statement.h"
#include "table/table.h"

namespace simple_olap
{

    class Catalog
    {
    public:
        // 应该不需要。
        // static std::unique_ptr<Catalog> Load(const std::filesystem::path &path);

        // ---------- Table Management ----------

        // 查找表：返回 table_id（仅查元数据，不要求表已载入内存），未找到时返回空 optional
        std::optional<TableId> FindTable(std::string_view table_name) const;

        // 创建新表：column_id 按列顺序从 0 分配
        bool CreateTable(const CreateTableStatement &stmt);

        bool LoadTable(const std::string &table_name);

        // 按表名获取表：不存在返回 nullptr；存在但未载入内存则自动 LoadTable
        Table *GetTable(const std::string &table_name);

        bool FlushTable(const std::string &table_name);

        // ---------- Read Metadata ----------

        CatalogMeta &GetCatalogMeta();

        /* 暂时先不管，应该放table里吧，放这里有问题
        void AddSegment(
            std::string_view table_name,
            SegmentMeta segment);
        */

        // 虽然用的不多，但还是应该保留创建功能。
        bool Create(const std::filesystem::path &path);

        bool LoadMeta(const std::filesystem::path &path);

        bool SaveMeta(const std::filesystem::path &path) const;

    private:
        CatalogMeta metadata_;
        std::unordered_map<uint32_t, std::unique_ptr<Table>> tables_ptr;
        std::filesystem::path root_path_;
    };

} // namespace simple_olap
