#pragma once

#include <memory>
#include "datastructs.h"
#include "table/table.h"

namespace simple_olap
{

    class Catalog
    {
    public:
        // 应该不需要。
        // static std::unique_ptr<Catalog> Load(const std::filesystem::path &path);

        // // ---------- Table Managment ----------

        // const std::unique_ptr<Table> FindTable(std::string_view table_name) const;

        // 调用Tbale::Create()
        bool CreateTable(const std::string &table_name, TableSchema schema);

        bool LoadTable(const std::string &table_name);

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
    };

} // namespace simple_olap
