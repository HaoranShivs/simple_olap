#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>

namespace simple_olap {

enum class DataType : uint8_t {
    INVALID = 0,
    INT32,
    INT64,
    FLOAT,
    DOUBLE,
    VARCHAR,
};

// 比较运算符：公共定义，供存储层 Condition 与 SQL 层共用
enum class CmpOp : uint8_t { EQ, NE, GT, GE, LT, LE };

// 公共 ID 类型别名：供存储层、执行层、计划层共用
using TableId = uint32_t;
using ColumnId = uint32_t;
using SegmentId = uint32_t;

// 聚合函数类型：公共定义，供 SQL AST 与执行层共用
enum class AggType : uint8_t {
    INVALID = 0,
    SUM,   // 求和
    COUNT, // 计数
    AVG,   // 平均值（内部用 SUM + COUNT 实现）
    MIN,   // 最小值
    MAX    // 最大值
};

// ==========================================
// VARCHAR 内联定长槽（DuckDB string_t 的简化版）
// ==========================================
// 每个值占固定 64 字节：[4 字节长度][60 字节内联负载]。
// 定长槽使得 Segment / ColumnChunk / mmap 的 offset = row * elem_size
// 布局对 VARCHAR 同样成立，存储层无需区分定长/变长两种列格式。
// 超过 60 字节的字符串 v1 暂不支持（后续可扩展 overflow block）。
constexpr size_t kVarcharSlotSize = 64;
constexpr size_t kVarcharInlineMax = kVarcharSlotSize - sizeof(uint32_t);

// 把字符串写入定长槽（长度前缀 + 内联负载 + 零填充）
inline void WriteVarcharSlot(uint8_t* slot, const std::string& value) {
    const uint32_t len = static_cast<uint32_t>(value.size());
    if (len > kVarcharInlineMax) {
        throw std::runtime_error("VARCHAR value too long for inline slot (max 60 bytes)");
    }
    std::memcpy(slot, &len, sizeof(uint32_t));
    std::memcpy(slot + sizeof(uint32_t), value.data(), len);
    std::memset(slot + sizeof(uint32_t) + len, 0, kVarcharInlineMax - len);
}

// 读取槽内字符串长度（不拷贝负载）
inline uint32_t ReadVarcharLength(const uint8_t* slot) {
    uint32_t len = 0;
    std::memcpy(&len, slot, sizeof(uint32_t));
    return len > kVarcharInlineMax ? static_cast<uint32_t>(kVarcharInlineMax) : len; // 防御损坏数据
}

// 读取槽内字符串
inline std::string ReadVarcharSlot(const uint8_t* slot) {
    const uint32_t len = ReadVarcharLength(slot);
    return std::string(reinterpret_cast<const char*>(slot + sizeof(uint32_t)), len);
}

// 获取类型对应的元素大小（字节）
inline size_t TypeElemSize(DataType t) {
    switch (t) {
    case DataType::INT32:
        return sizeof(int32_t);
    case DataType::INT64:
        return sizeof(int64_t);
    case DataType::FLOAT:
        return sizeof(float);
    case DataType::DOUBLE:
        return sizeof(double);
    case DataType::VARCHAR:
        return kVarcharSlotSize;
    default:
        return sizeof(int64_t); // 默认按 INT64 处理
    }
}

} // namespace simple_olap