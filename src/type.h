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