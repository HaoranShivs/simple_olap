#pragma once

#include <cstddef>
#include <cstdint>

namespace simple_olap{

enum class DataType: uint8_t{
    INVALID = 0,
    INT32,
    INT64,
    FLOAT,
    DOUBLE,
    VARCHAR,
};

// 比较运算符：公共定义，供存储层 Condition 与 SQL 层共用
enum class CmpOp : uint8_t {
    EQ, NE, GT, GE, LT, LE
};

// 聚合函数类型：公共定义，供 SQL AST 与执行层共用
enum class AggType : uint8_t {
    INVALID = 0,
    SUM,   // 求和
    COUNT, // 计数
    AVG,   // 平均值（内部用 SUM + COUNT 实现）
    MIN,   // 最小值
    MAX    // 最大值
};

// 获取类型对应的元素大小（字节）
inline size_t TypeElemSize(DataType t)
{
    switch (t)
    {
    case DataType::INT32:
        return sizeof(int32_t);
    case DataType::INT64:
        return sizeof(int64_t);
    case DataType::FLOAT:
        return sizeof(float);
    case DataType::DOUBLE:
        return sizeof(double);
    default:
        return sizeof(int64_t); // 默认按 INT64 处理
    }
}

}   // namespace simple_olap