#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "../storage/datachunk.h"
#include "../type.h"

namespace simple_olap {

struct ResultColumn {
    std::string name;
    DataType type;
};

class QueryResult {
  public:
    enum class Type { SELECT, INSERT, CREATE_TABLE };

    Type type;

    std::vector<ResultColumn> columns;

    // SELECT result
    std::vector<DataChunk> chunks;

    // INSERT / DDL
    uint64_t affected_rows = 0;
};

} // namespace simple_olap
