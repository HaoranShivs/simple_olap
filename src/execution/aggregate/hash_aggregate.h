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
    // ---- 聚合中间态 ----
    // 并行执行时部分 group 表要跨线程传递（worker -> 全局合并阶段），
    // 因此公开这些类型；语义与单线程路径完全一致。
    struct GroupKey {
        std::vector<ExecValue> values;

        bool operator==(const GroupKey& other) const {
            return values == other.values;
        }
    };

    struct GroupKeyHash {
        size_t operator()(const GroupKey& key) const;
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

    HashAggregateOperator(std::unique_ptr<Operator> child, std::vector<ExecExprPtr> group_exprs,
                          std::vector<AggCallSpec> agg_calls, std::vector<AggregateOutputSpec> outputs)
        : child_(std::move(child)), group_exprs_(std::move(group_exprs)), agg_calls_(std::move(agg_calls)),
          outputs_(std::move(outputs)) {}

    void Init() override;
    bool Next(VectorBatch& output) override;

    // ---- 并行聚合支持（ParallelExecutionEngine 使用） ----

    // 聚合前阶段（多线程）：消费本 worker 分到的全部输入，
    // 形成本地部分 group 表（不 finalize、不产出 batch）。
    void ComputePartialGroups();

    // 移出本地部分 group 表（worker -> 全局合并阶段传递）
    GroupTable TakeGroups() {
        return std::move(groups_);
    }

    // 聚合后阶段（单线程）：把一份部分 group 表按聚合语义合并进当前表。
    // SUM/COUNT/AVG 在中间态（sum/count）上合并，AVG 语义正确；
    // MIN/MAX 取极值。
    void MergeGroups(GroupTable other);

    // 声明 group 表由外部注入（MergeGroups），
    // Next() 跳过 ConsumeAll，直接进入 emit 阶段。
    void SetExternalGroups() {
        external_groups_ = true;
    }

  private:
    StateVector MakeStates() const;
    GroupKey EvalGroupKey(const VectorBatch& batch, uint32_t physical_row) const;
    void UpdateAggregate(const AggCallSpec& call, AggState& state, const VectorBatch& batch, uint32_t physical_row);
    ExecValue FinalizeAggregate(const AggCallSpec& call, const AggState& state) const;
    // 按聚合语义合并单个中间态（MergeGroups 的逐列实现）
    void MergeAggState(const AggCallSpec& call, AggState& dst, const AggState& src) const;
    void ConsumeAll();

    std::unique_ptr<Operator> child_;
    std::vector<ExecExprPtr> group_exprs_;
    std::vector<AggCallSpec> agg_calls_;
    std::vector<AggregateOutputSpec> outputs_;

    VectorBatch input_{true};
    GroupTable groups_;
    GroupTable::iterator emit_it_{};
    bool consumed_ = false;

    // group 表由外部注入（并行执行的全局合并阶段）时为 true
    bool external_groups_ = false;
};

} // namespace simple_olap
