#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "../expression/exec_expression.h"
#include "../vector/vector.h"

namespace simple_olap {

// 一次聚合调用：聚合类型 + 参数表达式 + 结果类型。
struct AggCallSpec {
    AggType type = AggType::INVALID;
    ExecExprPtr arg; // nullptr 表示 COUNT(*)
    DataType result_type = DataType::INVALID;
};

// 聚合算子输出列的定义：指明该列是某个 GROUP BY 表达式还是某个聚合结果。
struct AggregateOutputSpec {
    enum class Kind : uint8_t {
        GROUP_KEY, // 输出 GROUP BY 表达式
        AGGREGATE, // 输出聚合结果
    };

    Kind kind = Kind::GROUP_KEY;
    uint32_t index = 0; // kind 对应的下标：GROUP_KEY 用 group_exprs，AGGREGATE 用 agg_calls
    DataType type = DataType::INVALID;
    std::string name;
};

// 聚合中间态与算法：与 Operator 外壳（HashAggregateOperator）解耦，
// 串行路径与并行 worker 共用同一份实现。
class HashAggregateState {
  public:
    // ---- 聚合中间态类型（跨线程传递部分 group 表时使用） ----
    // 一个分组键：各 GROUP BY 表达式的求值结果。
    struct GroupKey {
        std::vector<ExecValue> values;

        bool operator==(const GroupKey& other) const {
            return values == other.values;
        }
    };

    struct GroupKeyHash {
        size_t operator()(const GroupKey& key) const;
    };

    // 单个 group 内某个聚合调用的中间态。sum/count 供 COUNT/SUM/AVG 复用，
    // min_value/max_value 供 MIN/MAX 复用；has_value 表示是否已见过有效输入。
    struct AggState {
        long double sum = 0.0L;
        int64_t count = 0;
        bool has_value = false;
        ExecValue min_value = int32_t{0};
        ExecValue max_value = int32_t{0};
    };

    using StateVector = std::vector<AggState>; // 一个 group 的各聚合中间态
    using GroupTable = std::unordered_map<GroupKey, StateVector, GroupKeyHash>;

    // group_exprs / agg_calls 由调用方保证比 state 活得久（计划树持有）
    HashAggregateState(const std::vector<ExecExprPtr>* group_exprs, const std::vector<AggCallSpec>* agg_calls)
        : group_exprs_(group_exprs), agg_calls_(agg_calls) {}

    HashAggregateState(HashAggregateState&& other) noexcept = default;
    HashAggregateState& operator=(HashAggregateState&& other) noexcept = default;

    // 消费一个批次：把所有有效行聚合进本地 group 表
    void Consume(VectorBatch& batch);

    // 按聚合语义合并另一份部分状态（worker -> 全局合并阶段）：
    //   COUNT : count += other.count
    //   SUM   : sum   += other.sum
    //   AVG   : sum   += other.sum, count += other.count
    //   MIN   : min(lhs, rhs)
    //   MAX   : max(lhs, rhs)
    void Merge(HashAggregateState&& other);

    // Finalize + emit：把 group 表按 BATCH_SIZE 分批物化输出。
    // 返回 false 表示全部组已输出（EOF）。
    bool NextResult(VectorBatch& output);

    // 无 GROUP BY 时调用：预置一个全局空组，
    // 保证空输入时也能产出一个（空聚合值的）结果行。
    void EnsureGlobalGroup() {
        if (group_exprs_->empty()) {
            groups_.try_emplace(GroupKey{}, MakeStates());
        }
    }

    const std::vector<AggregateOutputSpec>* outputs() const {
        return outputs_;
    }

    void set_outputs(const std::vector<AggregateOutputSpec>* outputs) {
        outputs_ = outputs;
    }

  private:
    StateVector MakeStates() const;
    GroupKey EvalGroupKey(const VectorBatch& batch, uint32_t physical_row) const;
    static void UpdateAggregate(const AggCallSpec& call, AggState& state, const VectorBatch& batch,
                                uint32_t physical_row);
    static ExecValue FinalizeAggregate(const AggCallSpec& call, const AggState& state);
    static void CombineAggregate(const AggCallSpec& call, AggState& dst, const AggState& src);

    const std::vector<ExecExprPtr>* group_exprs_ = nullptr;
    const std::vector<AggCallSpec>* agg_calls_ = nullptr;
    const std::vector<AggregateOutputSpec>* outputs_ = nullptr;

    GroupTable groups_;
    GroupTable::iterator emit_it_{};
    bool emit_started_ = false;
};

} // namespace simple_olap
