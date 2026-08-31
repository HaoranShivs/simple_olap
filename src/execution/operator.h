#pragma once

#include "vector/vector.h"

namespace simple_olap
{
    /// @brief 算子抽象基类（Pull-based Volcano 模型）
    /// 所有算子通过 Init() + 循环调用 Next() 来驱动执行。
    /// Next() 返回 true 表示成功填充了一个 batch，false 表示数据耗尽。
    /// 【伏笔】：第二阶段中，Filter 和 Aggregate 的 Next() 内部
    ///           将使用 AVX2 Intrinsics 替换标量循环。
    class Operator
    {
    public:
        virtual ~Operator() = default;

        /// 初始化算子状态（每次查询开始时调用一次）
        virtual void Init() = 0;

        /// 拉取下一批数据填充到 batch 中
        /// @return true = 有数据, false = 数据流结束
        virtual bool Next(VectorBatch &batch) = 0;
    };
} // namespace simple_olap
