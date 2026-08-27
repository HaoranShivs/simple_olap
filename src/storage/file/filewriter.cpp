#include "filewriter.h"

namespace simple_olap
{

    FileWriter::~FileWriter()
    {
        if (file_ != nullptr)
        {
            std::fclose(file_);
            file_ = nullptr;
        }
    }

    bool FileWriter::Open(const std::filesystem::path &path)
    {
        if (file_ != nullptr)
        {
            Close();
        }
        file_ = std::fopen(path.c_str(), "wb");
        return file_ != nullptr;
    }

    bool FileWriter::Close()
    {
        if (file_ == nullptr)
        {
            return false;
        }
        int ret = std::fclose(file_);
        file_ = nullptr;
        return ret == 0;
    }

    bool FileWriter::Write(const void *data, size_t size)
    {
        if (file_ == nullptr || (size == 0))
        {
            return file_ != nullptr && size == 0;
        }
        return std::fwrite(data, 1, size, file_) == size;
    }

    uint64_t FileWriter::Tell() const
    {
        if (file_ == nullptr)
        {
            return 0;
        }
        long pos = std::ftell(file_);
        return pos < 0 ? 0 : static_cast<uint64_t>(pos);
    }

    bool FileWriter::Seek(uint64_t pos)
    {
        if (file_ == nullptr)
        {
            return false;
        }
        return std::fseek(file_, static_cast<long>(pos), SEEK_SET) == 0;
    }

} // namespace simple_olap
