#pragma once

#include <cstdint>
#include <future>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "../../parallel/thread_pool/thread_pool.h"
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

    // 开启并行模式：注入线程池与各 worker 的“聚合前”子树
    // （每个 worker 一棵独立的 child 树 + 部分 group 表聚合算子）。
    // 之后 Next() -> ConsumeAll() 会把各 worker 的 ComputePartialGroups()
    // 提交到线程池（Submit），部分 group 表经 future 传回。
    void SetupParallel(ThreadPool* pool, std::vector<std::unique_ptr<HashAggregateOperator>> workers) {
        pool_ = pool;
        workers_ = std::make_unique<std::vector<std::unique_ptr<HashAggregateOperator>>>(std::move(workers));
        parallel_ = pool_ != nullptr && workers_ != nullptr && !workers_->empty();
    }

    // 聚合前阶段（多线程，在线程池 worker 上执行）：消费本 worker 分到的
    // 全部输入，形成本地部分 group 表（不 finalize、不产出 batch）。
    void ComputePartialGroups();

    // 移出本地部分 group 表（worker -> 全局合并阶段传递）
    GroupTable TakeGroups() {
        return std::move(groups_);
    }

    // 聚合后阶段（单线程）：把一份部分 group 表按聚合语义合并进当前表。
    // 合并前先 DrainFutures()：对每个 worker 任务 future.get() 收齐部分表。
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
    // 把一份部分 group 表合并进 groups_（不含 future 处理）
    void MergeTableInto(GroupTable& src);
    // 并行模式：把各 worker 的 ComputePartialGroups 提交线程池（Submit）
    void SubmitPartialWorkers();
    // future.get() 收齐所有 worker 的部分 group 表并合并进 groups_
    void DrainFutures();
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

    // ---- 并行聚合状态 ----
    ThreadPool* pool_ = nullptr;
    // worker 算子容器：Submit 时逐个 move 进 task，提交完后清空
    std::unique_ptr<std::vector<std::unique_ptr<HashAggregateOperator>>> workers_;
    // 各 worker 部分 group 表的 future（ConsumeAll 填充，DrainFutures 消费）
    std::vector<std::future<GroupTable>> futures_;
    bool parallel_ = false;
};

} // namespace simple_olap
