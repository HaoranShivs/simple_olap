#pragma once

#include "../../type.h"
#include "../operator.h"
#include <cstdint>
#include <memory>
#include <optional>
#include <unordered_map>
#include <vector>

namespace simple_olap {
/// 单个聚合函数的描述
struct AggFunction {
    enum class Type : uint8_t { SUM, COUNT, MIN, MAX, AVG };

    Type type = Type::SUM;
    uint32_t col_index = 0; // 聚合作用的列（在子算子输出 batch 中的下标）
};

/// @brief 聚合算子（Blocking Operator）
///
/// 【设计要点】：
/// 1. Blocking 语义：必须消费所有输入数据后才能产出结果
/// 2. 支持多路聚合：一次查询可以计算多个聚合函数
/// 3. SIMD 优化：使用 AVX2 指令集加速 SUM/MIN/MAX 计算
/// 4. Selection Vector 支持：根据 sel_vector 只处理有效行
class AggregateOperator : public Operator {
  public:
    AggregateOperator(std::unique_ptr<Operator> child, std::vector<AggFunction> agg_funcs);

    void Init() override;

    bool Next(VectorBatch& batch) override;

    // ---- 结果访问器 ----
    double GetSumResult(size_t agg_idx) const;
    int64_t GetCountResult(size_t agg_idx) const;
    double GetAvgResult(size_t agg_idx) const;
    int32_t GetMinResult(size_t agg_idx) const;
    int32_t GetMaxResult(size_t agg_idx) const;
    int64_t GetTotalRows() const {
        return total_row_count_;
    }

  private:
    // 消费子算子的所有 batch，累加聚合结果
    void ConsumeAll();

    // 标量版聚合（Baseline）
    void AggregateScalar(const VectorBatch& batch);

    // AVX2 向量化版聚合（优化版）
    void AggregateAVX2(const VectorBatch& batch);

    std::unique_ptr<Operator> child_;
    std::vector<AggFunction> agg_funcs_;

    // 聚合中间状态（与 agg_funcs_ 一一对应）
    struct AggregateState {
        double sum = 0.0;     // SUM/AVG 的累加和
        int64_t count = 0;    // COUNT 计数（AVG 的分母）
        bool has_min = false; // 是否出现过有效值（MIN 初始化用）
        int32_t min = 0;      // 最小值
        bool has_max = false; // 是否出现过有效值（MAX 初始化用）
        int32_t max = 0;      // 最大值
    };

    std::vector<AggregateState> states_;

    bool consumed_ = false;
    bool result_emitted_ = false;

    int64_t total_row_count_ = 0;
};

struct AggCall {
    AggType type;

    // nullopt = COUNT(*)
    std::optional<uint32_t> input_slot;

    DataType result_type;
};

struct AggregateSpec {
    std::vector<uint32_t> group_by_slots;

    std::vector<AggCall> aggregates;
};

/// @brief 哈希聚合算子（Blocking Operator，支持 GROUP BY）
///
/// 【设计要点】：
/// 1. Blocking 语义：必须消费所有输入数据后才能产出结果
/// 2. 分组键由 group_by_slots 指定（子算子输出 batch 中的列下标）
/// 3. 哈希表：分组键编码为 int64 向量 -> 聚合中间状态
/// 4. Selection Vector 支持：根据 sel_vector 只处理有效行
class HashAggregateOperator : public Operator {
  public:
    HashAggregateOperator(std::unique_ptr<Operator> child, AggregateSpec spec);

    void Init() override;

    bool Next(VectorBatch& output) override;

  private:
    // 消费子算子的所有 batch，填充哈希表
    void ConsumeAll();

    // 分组键哈希函数（FNV-1a 变体，逐元素混合）
    struct GroupKeyHash {
        size_t operator()(const std::vector<int64_t>& key) const;
    };

    // 单个分组的聚合中间状态
    struct AggState {
        double sum = 0.0;     // SUM/AVG 的累加和
        int64_t count = 0;    // COUNT 计数（AVG 的分母）
        bool has_min = false; // 是否出现过有效值（MIN 初始化用）
        double min = 0.0;     // 最小值
        bool has_max = false; // 是否出现过有效值（MAX 初始化用）
        double max = 0.0;     // 最大值
    };

    std::unique_ptr<Operator> child_;

    AggregateSpec spec_;

    // 哈希表：分组键 -> 聚合状态
    std::unordered_map<std::vector<int64_t>, AggState, GroupKeyHash> groups_;

    // 分组键的插入顺序（保证输出顺序确定）
    std::vector<const std::vector<int64_t>*> group_order_;

    // 分组列的类型（第一次消费输入时记录，用于构建输出 batch）
    std::vector<DataType> group_types_;

    bool consumed_ = false;
    bool emitted_ = false;
};
} // namespace simple_olap
