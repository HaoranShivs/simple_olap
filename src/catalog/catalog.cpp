#include "catalog.h"

#include <fstream>
#include <system_error>

namespace simple_olap {
// 元数据文件约定：位于 catalog 根目录下的 catalog.meta
// Create / LoadMeta / SaveMeta 均以 root_path_ 为 catalog 根目录。
//
// 格式 v3（当前）：
//   [magic "COL3"][entry 数量]{ [name][table_id][TableSchema] }...
//   TableSchema = 列定义 + 主键 + 二级键；schema/键的权威来源是 Catalog。
// 格式 v2（旧，只读兼容）：
//   [magic "COL2"][entry 数量]{ [name][table_id][只含列的 TableSchema] }...
//   键定义留空。
// 格式 v1（旧，只读兼容）：
//   [entry 数量]{ [name][table_id] }...
//   schema 留空（旧库的 table.meta 是已废弃的 TableMeta 格式，不再回读）。
constexpr uint32_t kCatalogMetaMagicV2 = 0x324C4F43; // "COL2" 小端
constexpr uint32_t kCatalogMetaMagicV3 = 0x334C4F43; // "COL3" 小端

// ---------- 持久化 ----------

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

    // 3. 反序列化：
    //    v3 带 magic + 完整键定义；v2 带 magic 但只有列；
    //    v1 旧格式只存 name -> id 映射，schema 留空（保持只读兼容）。
    try {
        BinaryReader reader(buffer);

        name_index_.clear();
        tables_.clear();

        const bool has_magic = buffer.size() >= sizeof(uint32_t);
        const uint32_t magic = has_magic ? reader.ReadUInt32() : 0;

        if (has_magic && magic == kCatalogMetaMagicV3) {
            // v3：magic + 完整条目（列 + 键）
            const uint32_t count = reader.ReadUInt32();
            for (uint32_t i = 0; i < count; ++i) {
                TableCatalogEntry entry = TableCatalogEntry::Deserialize(reader);

                name_index_[entry.name] = entry.table_id;
                tables_[entry.table_id] = std::move(entry);
            }
        } else if (has_magic && magic == kCatalogMetaMagicV2) {
            // v2：magic + 只含列定义的条目，键（primary_key / secondary_keys）留空
            const uint32_t count = reader.ReadUInt32();
            for (uint32_t i = 0; i < count; ++i) {
                TableCatalogEntry entry = TableCatalogEntry::DeserializeLegacy(reader);

                name_index_[entry.name] = entry.table_id;
                tables_[entry.table_id] = std::move(entry);
            }
        } else {
            // v1：CatalogMeta（name -> id 映射），schema 留空
            reader.SetFromVector(buffer);
            CatalogMeta meta = CatalogMeta::Deserialize(reader);

            for (const auto& kv : meta.table_name_id) {
                TableCatalogEntry entry;
                entry.table_id = kv.second;
                entry.name = kv.first;
                // v1 库的 table.meta 是已废弃的 TableMeta 格式，不再回读 schema；
                // schema 为空的表 GetTable 仍可用（scan 会失败），不让单表损坏拖垮整个 catalog

                name_index_[entry.name] = entry.table_id;
                tables_[entry.table_id] = std::move(entry);
            }
        }
    } catch (const std::exception&) {
        // 文件损坏或格式不匹配
        return false;
    }

    root_path_ = path;
    return true;
}

bool Catalog::SaveMeta() const {
    // v3 格式：magic + 完整条目（列 + 键，序列化逻辑在 TableCatalogEntry / TableSchema 内部）
    BinaryWriter writer;
    writer.WriteUInt32(kCatalogMetaMagicV3);
    writer.WriteUInt32(static_cast<uint32_t>(tables_.size()));
    for (const auto& kv : tables_) {
        kv.second.Serialize(writer);
    }

    const std::filesystem::path meta_path = root_path_ / "catalog.meta";
    std::ofstream file(meta_path, std::ios::binary | std::ios::trunc);
    if (!file) {
        return false;
    }

    const auto& buffer = writer.GetBuffer();
    file.write(reinterpret_cast<const char*>(buffer.data()), static_cast<std::streamsize>(buffer.size()));

    return file.good();
}

// ---------- 表变更 ----------

bool Catalog::CreateTable(const std::string& table_name, const TableSchema& schema) {
    // 1. 重名检查（列名 / 键定义已由 Binder 校验）
    if (name_index_.count(table_name) > 0) {
        return false;
    }

    // 2. 组装目录条目并登记（table_id 自动分配）
    TableCatalogEntry entry;
    entry.table_id = NextTableId();
    entry.name = table_name;
    entry.schema = schema;

    name_index_[table_name] = entry.table_id;
    tables_[entry.table_id] = std::move(entry);

    // 3. 立即持久化元数据；失败则回滚内存状态
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

// ---------- 查询 ----------

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

// ---------- 内部辅助 ----------

TableId Catalog::NextTableId() const {
    TableId next = 0;
    for (const auto& kv : tables_) {
        if (kv.first >= next) {
            next = kv.first + 1;
        }
    }
    return next;
}

} // namespace simple_olap
