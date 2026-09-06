#pragma once

#include "../execution_context.h"
#include "../operator.h"
#include "../../storage/datastructs.h"

namespace simple_olap {

class Table;

class SeqScanOperator final : public Operator {
public:
    SeqScanOperator(TableId table_id,
                    ScanOptions options,
                    ExecutionContext *ctx)
        : table_id_(table_id), options_(std::move(options)), ctx_(ctx) {}

    void Init() override;
    bool Next(VectorBatch &batch) override;

private:
    TableId table_id_;
    ScanOptions options_;
    ExecutionContext *ctx_ = nullptr;
    Table *table_ = nullptr;
    ScanCursor cursor_{};
};

} // namespace simple_olap
