#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>

namespace simple_olap
{
    // 顺序二进制写入器：封装文件句柄，记录当前写入位置，
    // 供 ColumnBuilder / SegmentBuilder 落盘时使用。
    class FileWriter
    {
    public:
        FileWriter() = default;

        // 禁止拷贝
        FileWriter(const FileWriter &) = delete;
        FileWriter &operator=(const FileWriter &) = delete;

        ~FileWriter();

        // 以截断方式打开（覆盖写）
        bool Open(const std::filesystem::path &path);

        // 刷新缓冲区并关闭文件
        bool Close();

        // 写入原始字节，成功返回 true
        bool Write(const void *data, size_t size);

        // 当前写入位置（字节偏移）
        uint64_t Tell() const;

        // 重新定位写入位置（用于回填元数据区）
        bool Seek(uint64_t pos);

    private:
        std::FILE *file_ = nullptr;
    };

} // namespace simple_olap
