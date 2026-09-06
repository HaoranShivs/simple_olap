#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "../expression/exec_expression.h"
#include "../operator.h"

namespace simple_olap {

struct AggCallSpec {
    AggType type = AggType::INVALID;
    ExecExprPtr arg; // nullptr means COUNT(*)
    DataType result_type = DataType::INVALID;
};

struct AggregateOutputSpec {
    enum class Kind : uint8_t {
        GROUP_KEY,
        AGGREGATE,
    };

    Kind kind = Kind::GROUP_KEY;
    uint32_t index = 0;
    DataType type = DataType::INVALID;
    std::string name;
};

class HashAggregateOperator final : public Operator {
public:
    HashAggregateOperator(std::unique_ptr<Operator> child,
                          std::vector<ExecExprPtr> group_exprs,
                          std::vector<AggCallSpec> agg_calls,
                          std::vector<AggregateOutputSpec> outputs)
        : child_(std::move(child)),
          group_exprs_(std::move(group_exprs)),
          agg_calls_(std::move(agg_calls)),
          outputs_(std::move(outputs)) {}

    void Init() override;
    bool Next(VectorBatch &output) override;

private:
    struct GroupKey {
        std::vector<ExecValue> values;

        bool operator==(const GroupKey &other) const {
            return values == other.values;
        }
    };

    struct GroupKeyHash {
        size_t operator()(const GroupKey &key) const;
    };

    struct AggState {
        long double sum = 0.0L;
        int64_t count = 0;
        bool has_value = false;
        ExecValue min_value = int32_t{0};
        ExecValue max_value = int32_t{0};
    };

    using StateVector = std::vector<AggState>;
    using GroupTable = std::unordered_map<GroupKey, StateVector, GroupKeyHash>;

    StateVector MakeStates() const;
    GroupKey EvalGroupKey(const VectorBatch &batch, uint32_t physical_row) const;
    void UpdateAggregate(const AggCallSpec &call,
                         AggState &state,
                         const VectorBatch &batch,
                         uint32_t physical_row);
    ExecValue FinalizeAggregate(const AggCallSpec &call, const AggState &state) const;
    void ConsumeAll();

    std::unique_ptr<Operator> child_;
    std::vector<ExecExprPtr> group_exprs_;
    std::vector<AggCallSpec> agg_calls_;
    std::vector<AggregateOutputSpec> outputs_;

    VectorBatch input_{true};
    GroupTable groups_;
    GroupTable::iterator emit_it_{};
    bool consumed_ = false;
};

} // namespace simple_olap
