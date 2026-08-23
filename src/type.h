#pragma once

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

}   // namespace simple_olap