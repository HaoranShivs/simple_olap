#include "catalog.h"

#include <fstream>
#include <system_error>

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

        return true;
    }

} // namespace simple_olap