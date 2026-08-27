#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>

namespace simple_olap
{
    // 只读内存映射文件：用 mmap 把整个文件映射进虚拟地址空间，
    // 由操作系统按需换页，避免显式 read 带来的用户态拷贝。
    class MappedFile
    {
    public:
        MappedFile() = default;

        MappedFile(const MappedFile &) = delete;
        MappedFile &operator=(const MappedFile &) = delete;

        MappedFile(MappedFile &&other) noexcept;
        MappedFile &operator=(MappedFile &&other) noexcept;

        ~MappedFile();

        // 打开并映射文件；失败返回 std::nullopt
        static std::optional<MappedFile>
        OpenReadOnly(const std::filesystem::path &path);

        const std::byte *data() const noexcept
        {
            return data_;
        }

        size_t size() const noexcept
        {
            return size_;
        }

        bool valid() const noexcept
        {
            return data_ != nullptr;
        }

    private:
        // 解除映射
        void Close() noexcept;

        const std::byte *data_ = nullptr;
        size_t size_ = 0;
    };

} // namespace simple_olap
