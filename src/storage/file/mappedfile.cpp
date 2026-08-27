#include "mappedfile.h"

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

namespace simple_olap
{
    MappedFile::MappedFile(MappedFile &&other) noexcept
        : data_(other.data_), size_(other.size_)
    {
        other.data_ = nullptr;
        other.size_ = 0;
    }

    MappedFile &MappedFile::operator=(MappedFile &&other) noexcept
    {
        if (this != &other)
        {
            Close();
            data_ = other.data_;
            size_ = other.size_;
            other.data_ = nullptr;
            other.size_ = 0;
        }
        return *this;
    }

    MappedFile::~MappedFile()
    {
        Close();
    }

    void MappedFile::Close() noexcept
    {
        if (data_ != nullptr)
        {
            ::munmap(const_cast<std::byte *>(data_), size_);
            data_ = nullptr;
            size_ = 0;
        }
    }

    std::optional<MappedFile> MappedFile::OpenReadOnly(const std::filesystem::path &path)
    {
        // 1. 以只读方式打开文件
        int fd = ::open(path.c_str(), O_RDONLY);
        if (fd < 0)
        {
            return std::nullopt;
        }

        // 2. 获取文件大小
        struct stat st;
        if (::fstat(fd, &st) != 0 || st.st_size <= 0)
        {
            ::close(fd);
            return std::nullopt;
        }

        // 3. 映射整个文件到虚拟地址空间（只读、私有映射，写时复制）
        void *addr = ::mmap(nullptr, static_cast<size_t>(st.st_size),
                            PROT_READ, MAP_PRIVATE, fd, 0);
        // 映射建立后 fd 即可关闭，映射仍然有效
        ::close(fd);

        if (addr == MAP_FAILED)
        {
            return std::nullopt;
        }

        MappedFile file;
        file.data_ = static_cast<const std::byte *>(addr);
        file.size_ = static_cast<size_t>(st.st_size);
        return file;
    }

} // namespace simple_olap
