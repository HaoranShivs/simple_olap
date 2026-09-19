#pragma once

#include <array>
#include <cstdint>
#include <memory_resource>
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
//
// 内存归属：所有 PMR 容器（GroupTable / GroupKey / StateVector）都从
// 构造时传入的 memory_resource（Arena）分配。worker 状态用 worker Arena，
// 全局状态用 coordinator Arena；Merge 时把 worker key 拷贝进 coordinator
// Arena，两个 Arena 完全解耦。
//
// 两种执行模式（构造时按 group_exprs 是否为空一次性确定）：
//
//   GLOBAL（无 GROUP BY）
//       不构造 GroupKey、不查 hash 表；聚合状态直接更新 global_states_。
//       COUNT(*) 在 batch 粒度聚合：count += active rows，而不是逐行 ++。
//       始终存在一个全局组，因此空输入也产出一行（COUNT=0 / SUM=0 / ...）。
//
//   GROUPED_HASH（有 GROUP BY）
//       原 GroupTable（pmr::unordered_map<GroupKey, StateVector>）路径，语义不变。
class HashAggregateState {
  public:
    enum class AggregateMode : uint8_t {
        GLOBAL,       // 无 GROUP BY
        GROUPED_HASH, // 有 GROUP BY
    };

    // ---- 聚合中间态类型（跨线程传递部分 group 表时使用） ----
    // 一个分组键：各 GROUP BY 表达式的求值结果。
    struct GroupKey {
        explicit GroupKey(std::pmr::memory_resource* resource) : values(resource) {}

        std::pmr::vector<ExecValue> values;

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

    using StateVector = std::pmr::vector<AggState>; // 一个 group 的各聚合中间态
    using GroupTable = std::pmr::unordered_map<GroupKey, StateVector, GroupKeyHash>;

    // group_exprs / agg_calls 由调用方保证比 state 活得久（计划树持有）。
    // memory 为 PMR 资源（Arena），必须比 state 活得久。
    HashAggregateState(const std::vector<ExecExprPtr>* group_exprs, const std::vector<AggCallSpec>* agg_calls,
                       std::pmr::memory_resource* memory)
        : group_exprs_(group_exprs), agg_calls_(agg_calls), memory_(memory),
          mode_(group_exprs->empty() ? AggregateMode::GLOBAL : AggregateMode::GROUPED_HASH), groups_(memory),
          global_states_(memory), global_count_star_calls_(memory), global_row_calls_(memory) {
        if (mode_ == AggregateMode::GLOBAL) {
            // 预编译一次：把 COUNT(*)（batch 级聚合）与需要逐行求值的调用分开。
            global_states_.resize(agg_calls_->size());
            for (uint32_t i = 0; i < static_cast<uint32_t>(agg_calls_->size()); ++i) {
                const AggCallSpec& call = (*agg_calls_)[i];
                if (call.type == AggType::COUNT && call.arg == nullptr) {
                    global_count_star_calls_.push_back(i);
                } else {
                    global_row_calls_.push_back(i);
                }
            }
        }
    }

    HashAggregateState(HashAggregateState&& other) noexcept = default;
    HashAggregateState& operator=(HashAggregateState&& other) noexcept = default;

    // 消费一个批次：把所有有效行聚合进本地状态。
    void Consume(VectorBatch& batch);

    // 按聚合语义合并另一份部分状态（worker -> 全局合并阶段）：
    //   COUNT : count += other.count
    //   SUM   : sum   += other.sum
    //   AVG   : sum   += other.sum, count += other.count
    //   MIN   : min(lhs, rhs)
    //   MAX   : max(lhs, rhs)
    void Merge(HashAggregateState&& other);

    // Finalize + emit：
    //   GLOBAL       -> 最多输出 1 行（空输入也输出 COUNT=0 等空聚合值）
    //   GROUPED_HASH -> 把 group 表按 BATCH_SIZE 分批物化输出
    // 返回 false 表示全部结果已输出（EOF）。
    bool NextResult(VectorBatch& output);

    AggregateMode mode() const noexcept {
        return mode_;
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

    // ---- GLOBAL 模式 ----
    void ConsumeGlobal(VectorBatch& batch);
    void MergeGlobal(const HashAggregateState& other);
    bool NextGlobalResult(VectorBatch& output);

    // ---- GROUPED_HASH 模式 ----
    void ConsumeGrouped(VectorBatch& batch);
    void MergeGrouped(HashAggregateState&& other);
    bool NextGroupedResult(VectorBatch& output);

    const std::vector<ExecExprPtr>* group_exprs_ = nullptr;
    const std::vector<AggCallSpec>* agg_calls_ = nullptr;
    const std::vector<AggregateOutputSpec>* outputs_ = nullptr;

    // PMR 资源（Arena）：GroupTable / GroupKey / StateVector 的分配来源。
    std::pmr::memory_resource* memory_ = nullptr;

    // 构造时确定，热路径只在 batch 粒度分流一次。
    AggregateMode mode_ = AggregateMode::GLOBAL;

    // ---- GROUPED_HASH 状态 ----
    GroupTable groups_;
    GroupTable::iterator emit_it_{};
    bool emit_started_ = false;

    // ---- GLOBAL 状态 ----
    StateVector global_states_;
    // 仅含 COUNT(*) 的调用：batch 级聚合，不逐行更新。
    std::pmr::vector<uint32_t> global_count_star_calls_;
    // 其余调用（COUNT(col) / SUM / AVG / MIN / MAX）：逐 active row 求值。
    std::pmr::vector<uint32_t> global_row_calls_;
    bool global_emitted_ = false;
};

} // namespace simple_olap
