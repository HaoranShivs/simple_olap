#pragma once

#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

namespace simple_olap
{

    // ==========================================
    // BinaryWriter - 二进制写入器
    // ==========================================
    // 提供基础类型的序列化能力，不依赖具体业务数据结构。
    // 所有多字节整数采用小端序（little-endian）。

    class BinaryWriter
    {
    public:
        BinaryWriter() : buffer_() {}
        ~BinaryWriter() = default;

        // 禁止拷贝
        BinaryWriter(const BinaryWriter &) = delete;
        BinaryWriter &operator=(const BinaryWriter &) = delete;

        // 允许移动
        BinaryWriter(BinaryWriter &&) = default;
        BinaryWriter &operator=(BinaryWriter &&) = default;

        /**
         * 写入单字节无符号整数
         */
        void WriteUInt8(uint8_t value)
        {
            buffer_.push_back(value);
        }

        /**
         * 写入 32 位无符号整数（小端序）
         */
        void WriteUInt32(uint32_t value)
        {
            for (uint8_t i = 0; i < 4; ++i)
            {
                buffer_.push_back(static_cast<uint8_t>((value >> (i * 8)) & 0xFF));
            }
        }

        /**
         * 写入 64 位无符号整数（小端序）
         */
        void WriteUInt64(uint64_t value)
        {
            for (uint8_t i = 0; i < 8; ++i)
            {
                buffer_.push_back(static_cast<uint8_t>((value >> (i * 8)) & 0xFF));
            }
        }

        /**
         * 写入 32 位有符号整数（小端序）
         */
        void WriteInt32(int32_t value)
        {
            WriteUInt32(static_cast<uint32_t>(value));
        }

        /**
         * 写入 64 位有符号整数（小端序）
         */
        void WriteInt64(int64_t value)
        {
            WriteUInt64(static_cast<uint64_t>(value));
        }

        /**
         * 写入 32 位浮点数（小端序）
         */
        void WriteFloat(float value)
        {
            static_assert(sizeof(float) == 4, "float must be 4 bytes");
            uint32_t int_value;
            std::memcpy(&int_value, &value, sizeof(float));
            WriteUInt32(int_value);
        }

        /**
         * 写入 64 位双精度浮点数（小端序）
         */
        void WriteDouble(double value)
        {
            static_assert(sizeof(double) == 8, "double must be 8 bytes");
            uint64_t long_value;
            std::memcpy(&long_value, &value, sizeof(double));
            WriteUInt64(long_value);
        }

        /**
         * 写入字符串（先写长度，再写内容）
         */
        void WriteString(const std::string &value)
        {
            WriteUInt32(static_cast<uint32_t>(value.size()));
            buffer_.insert(buffer_.end(), value.begin(), value.end());
        }

        /**
         * 写入原始数据
         */
        void WriteRaw(const void *data, size_t size)
        {
            const uint8_t *bytes = static_cast<const uint8_t *>(data);
            buffer_.insert(buffer_.end(), bytes, bytes + size);
        }

        /**
         * 获取缓冲区数据
         */
        const std::vector<uint8_t> &GetBuffer() const
        {
            return buffer_;
        }

        /**
         * 获取缓冲区大小
         */
        size_t GetSize() const
        {
            return buffer_.size();
        }

        /**
         * 清空缓冲区
         */
        void Clear()
        {
            buffer_.clear();
        }

    private:
        std::vector<uint8_t> buffer_;
    };

    // ==========================================
    // BinaryReader - 二进制读取器
    // ==========================================
    // 提供基础类型的反序列化能力，不依赖具体业务数据结构。
    // 所有多字节整数采用小端序（little-endian）。

    class BinaryReader
    {
    public:
        /**
         * 从已有数据构造 Reader
         */
        explicit BinaryReader(const uint8_t *data, size_t size)
            : data_(data), size_(size), offset_(0) {}

        /**
         * 从 vector 构造 Reader
         */
        explicit BinaryReader(const std::vector<uint8_t> &data)
            : data_(data.data()), size_(data.size()), offset_(0) {}

        ~BinaryReader() = default;

        // 禁止拷贝
        BinaryReader(const BinaryReader &) = delete;
        BinaryReader &operator=(const BinaryReader &) = delete;

        /**
         * 读取单字节无符号整数
         */
        uint8_t ReadUInt8()
        {
            if (offset_ + 1 > size_)
            {
                throw std::runtime_error("BinaryReader::ReadUInt8 - out of range");
            }
            uint8_t value = data_[offset_];
            offset_ += 1;
            return value;
        }

        /**
         * 读取 32 位无符号整数（小端序）
         */
        uint32_t ReadUInt32()
        {
            if (offset_ + 4 > size_)
            {
                throw std::runtime_error("BinaryReader::ReadUInt32 - out of range");
            }
            uint32_t value = 0;
            for (uint8_t i = 0; i < 4; ++i)
            {
                value |= static_cast<uint32_t>(data_[offset_ + i]) << (i * 8);
            }
            offset_ += 4;
            return value;
        }

        /**
         * 读取 64 位无符号整数（小端序）
         */
        uint64_t ReadUInt64()
        {
            if (offset_ + 8 > size_)
            {
                throw std::runtime_error("BinaryReader::ReadUInt64 - out of range");
            }
            uint64_t value = 0;
            for (uint8_t i = 0; i < 8; ++i)
            {
                value |= static_cast<uint64_t>(data_[offset_ + i]) << (i * 8);
            }
            offset_ += 8;
            return value;
        }

        /**
         * 读取 32 位有符号整数（小端序）
         */
        int32_t ReadInt32()
        {
            return static_cast<int32_t>(ReadUInt32());
        }

        /**
         * 读取 64 位有符号整数（小端序）
         */
        int64_t ReadInt64()
        {
            return static_cast<int64_t>(ReadUInt64());
        }

        /**
         * 读取 32 位浮点数（小端序）
         */
        float ReadFloat()
        {
            uint32_t int_value = ReadUInt32();
            float value;
            std::memcpy(&value, &int_value, sizeof(float));
            return value;
        }

        /**
         * 读取 64 位双精度浮点数（小端序）
         */
        double ReadDouble()
        {
            uint64_t long_value = ReadUInt64();
            double value;
            std::memcpy(&value, &long_value, sizeof(double));
            return value;
        }

        /**
         * 读取字符串
         */
        std::string ReadString()
        {
            uint32_t len = ReadUInt32();
            if (offset_ + len > size_)
            {
                throw std::runtime_error("BinaryReader::ReadString - out of range");
            }
            std::string result(reinterpret_cast<const char *>(data_ + offset_), len);
            offset_ += len;
            return result;
        }

        /**
         * 读取原始数据到缓冲区
         */
        void ReadRaw(void *buffer, size_t size)
        {
            if (offset_ + size > size_)
            {
                throw std::runtime_error("BinaryReader::ReadRaw - out of range");
            }
            std::memcpy(buffer, data_ + offset_, size);
            offset_ += size;
        }

        /**
         * 使用 vector 作为数据源
         */
        void SetFromVector(const std::vector<uint8_t> &data)
        {
            buffer_ = data;
            data_ = buffer_.data();
            size_ = buffer_.size();
            offset_ = 0;
        }

        /**
         * 获取当前读取位置
         */
        size_t GetOffset() const
        {
            return offset_;
        }

        /**
         * 检查是否读完
         */
        bool IsEof() const
        {
            return offset_ >= size_;
        }

        /**
         * 获取总数据大小
         */
        size_t GetSize() const
        {
            return size_;
        }

        /**
         * 获取数据指针
         */
        const uint8_t *GetData() const
        {
            return data_;
        }

        /**
         * 跳过指定字节数
         */
        void Skip(size_t bytes)
        {
            if (offset_ + bytes > size_)
            {
                throw std::runtime_error("BinaryReader::Skip - out of range");
            }
            offset_ += bytes;
        }

    private:
        const uint8_t *data_;         // 数据指针
        size_t size_;                 // 数据大小
        size_t offset_;               // 当前读取位置
        std::vector<uint8_t> buffer_; // 内部缓冲区（用于 SetFromVector）
    };

} // namespace simple_olap