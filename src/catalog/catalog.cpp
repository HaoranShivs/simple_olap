#include "catalog.h"

#include <fstream>
#include <system_error>
#include <unordered_set>

namespace simple_olap {
// 元数据文件约定：位于 catalog 根目录下的 catalog.meta
// Create / LoadMeta / SaveMeta 均以 root_path_ 为 catalog 根目录。

bool Catalog::Create(const std::filesystem::path& path) {
    // 1. 创建 catalog 根目录（含父目录）
    std::error_code ec;
    std::filesystem::create_directories(path, ec);
    if (ec) {
        return false;
    }

    // 2. 若元数据文件已存在，说明该 catalog 已创建过，拒绝覆盖
    const std::filesystem::path meta_path = path / "catalog.meta";
    if (std::filesystem::exists(meta_path, ec)) {
        return false;
    }

    // 3. 初始化空元数据并写入磁盘
    name_index_.clear();
    tables_.clear();
    root_path_ = path;

    return SaveMeta();
}

bool Catalog::LoadMeta(const std::filesystem::path& path) {
    // 1. 元数据文件必须存在
    const std::filesystem::path meta_path = path / "catalog.meta";
    std::error_code ec;
    if (!std::filesystem::exists(meta_path, ec) || ec) {
        return false;
    }

    // 2. 读入整个文件
    std::ifstream file(meta_path, std::ios::binary);
    if (!file) {
        return false;
    }

    std::vector<uint8_t> buffer((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    if (!file.good() && !file.eof()) {
        return false;
    }

    // 3. 反序列化：旧格式只存 name -> id 映射，schema 从各表的
    //    tables/{id}/table.meta 中补齐（保持磁盘格式向后兼容）
    try {
        BinaryReader reader(buffer);
        CatalogMeta meta = CatalogMeta::Deserialize(reader);

        name_index_.clear();
        tables_.clear();
        for (const auto& kv : meta.table_name_id) {
            const TableId table_id = kv.second;

            TableCatalogEntry entry;
            entry.table_id = table_id;
            entry.name = kv.first;

            // schema 权威来源：tables/{table_id}/table.meta
            const std::filesystem::path table_meta_path = path / "tables" / std::to_string(table_id) / "table.meta";
            std::ifstream table_file(table_meta_path, std::ios::binary);
            if (table_file) {
                std::vector<uint8_t> table_buffer((std::istreambuf_iterator<char>(table_file)),
                                                  std::istreambuf_iterator<char>());
                BinaryReader table_reader(table_buffer);
                TableMeta table_meta = TableMeta::Deserialize(table_reader);
                entry.schema = std::move(table_meta.schema);
            }
            // table.meta 缺失时 schema 为空，GetTable 仍可用（scan 会失败），
            // 不让单表损坏拖垮整个 catalog 加载

            name_index_[entry.name] = table_id;
            tables_[table_id] = std::move(entry);
        }
    } catch (const std::exception&) {
        // 文件损坏或格式不匹配
        return false;
    }

    root_path_ = path;
    return true;
}

bool Catalog::SaveMeta() const {
    // 序列化为旧格式（name -> id 映射），schema 留在 tables/{id}/table.meta
    CatalogMeta meta;
    for (const auto& kv : name_index_) {
        meta.table_name_id[kv.first] = kv.second;
    }

    BinaryWriter writer;
    meta.Serialize(writer);

    const std::filesystem::path meta_path = root_path_ / "catalog.meta";
    std::ofstream file(meta_path, std::ios::binary | std::ios::trunc);
    if (!file) {
        return false;
    }

    const auto& buffer = writer.GetBuffer();
    file.write(reinterpret_cast<const char*>(buffer.data()), static_cast<std::streamsize>(buffer.size()));

    return file.good();
}

TableId Catalog::NextTableId() const {
    TableId next = 0;
    for (const auto& kv : tables_) {
        if (kv.first >= next) {
            next = kv.first + 1;
        }
    }
    return next;
}

bool Catalog::CreateTable(const CreateTableStatement& stmt) {
    const std::string& table_name = stmt.table_name;
    const auto& columns = stmt.columns;

    // 1. 重名检查
    if (name_index_.count(table_name) > 0) {
        return false;
    }

    // 2. 检查列名重复
    std::unordered_set<std::string> seen_column_names;
    for (const auto& col : columns) {
        if (seen_column_names.count(col.name) > 0) {
            return false;
        }
        seen_column_names.insert(col.name);
    }

    // 3. 转换 columns -> TableSchema（column_id 按顺序从 0 分配）
    TableSchema schema;
    schema.columns.reserve(columns.size());
    for (uint32_t i = 0; i < static_cast<uint32_t>(columns.size()); ++i) {
        schema.columns.push_back(ColumnSchema{i, columns[i].name, columns[i].type});
    }

    // 4. 组装目录条目并登记（table_id 自动分配）
    TableCatalogEntry entry;
    entry.table_id = NextTableId();
    entry.name = table_name;
    entry.schema = std::move(schema);

    name_index_[table_name] = entry.table_id;
    tables_[entry.table_id] = std::move(entry);

    // 5. 立即持久化元数据；失败则回滚内存状态
    if (!SaveMeta()) {
        const TableId rolled_back_id = name_index_[table_name];
        name_index_.erase(table_name);
        tables_.erase(rolled_back_id);
        return false;
    }

    return true;
}

bool Catalog::DropTable(std::string_view table_name) {
    const auto it = name_index_.find(std::string(table_name));
    if (it == name_index_.end()) {
        return false;
    }

    const TableId table_id = it->second;
    name_index_.erase(it);
    tables_.erase(table_id);

    if (!SaveMeta()) {
        return false;
    }
    return true;
}

std::optional<TableId> Catalog::FindTable(std::string_view table_name) const {
    const auto it = name_index_.find(std::string(table_name));
    if (it == name_index_.end()) {
        return std::nullopt;
    }
    return it->second;
}

const TableCatalogEntry* Catalog::GetTable(std::string_view table_name) const {
    const auto it = name_index_.find(std::string(table_name));
    if (it == name_index_.end()) {
        return nullptr;
    }
    return &tables_.at(it->second);
}

const TableCatalogEntry* Catalog::GetTable(TableId table_id) const {
    const auto it = tables_.find(table_id);
    if (it == tables_.end()) {
        return nullptr;
    }
    return &it->second;
}

} // namespace simple_olap
