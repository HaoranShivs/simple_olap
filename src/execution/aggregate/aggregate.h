#pragma once

#include "../operator.h"
#include <memory>
#include <vector>

namespace simple_olap
{
    /// 单个聚合函数的描述
    struct AggFunction
    {
        enum class Type : uint8_t
        {
            SUM,
            COUNT,
            MIN,
            MAX,
            AVG
        };

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
    class AggregateOperator : public Operator
    {
    public:
        AggregateOperator(std::unique_ptr<Operator> child,
                          std::vector<AggFunction> agg_funcs);

        void Init() override;

        bool Next(VectorBatch &batch) override;

        // ---- 结果访问器 ----
        double GetSumResult(size_t agg_idx) const;
        int64_t GetCountResult(size_t agg_idx) const;
        double GetAvgResult(size_t agg_idx) const;
        int32_t GetMinResult(size_t agg_idx) const;
        int32_t GetMaxResult(size_t agg_idx) const;
        int64_t GetTotalRows() const { return total_row_count_; }

    private:
        // 消费子算子的所有 batch，累加聚合结果
        void ConsumeAll();

        // 标量版聚合（Baseline）
        void AggregateScalar(const VectorBatch &batch);

        // AVX2 向量化版聚合（优化版）
        void AggregateAVX2(const VectorBatch &batch);

        std::unique_ptr<Operator> child_;
        std::vector<AggFunction> agg_funcs_;

        // 聚合中间状态（与 agg_funcs_ 一一对应）
        struct AggregateState
        {
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
} // namespace simple_olap
