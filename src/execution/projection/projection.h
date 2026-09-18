#pragma once

#include <cstdint>
#include <memory>
#include <vector>

#include "../expression/exec_expression.h"
#include "../expression/vector_expression.h"
#include "../operator.h"
#include "../vector/vector.h"

namespace simple_olap {

// 单个输出列的投影计划：Init() 阶段一次性分类，热路径不再遍历表达式树。
struct ProjectionSlotPlan {
    enum class Kind : uint8_t {
        DIRECT_COLUMN,     // 直接列引用：零拷贝切列（沿用既有 view 投影）
        VECTOR_EXPRESSION, // 已编译成整列求值：dense 输入下走 SIMD/向量内核
        SCALAR_EXPRESSION, // 其余情况：逐行 Eval 物化
    };

    Kind kind = Kind::SCALAR_EXPRESSION;

    // kind == DIRECT_COLUMN 时的输入列下标
    uint32_t direct_slot = 0;

    // kind == VECTOR_EXPRESSION 时的已编译表达式（拥有所有权）
    std::unique_ptr<VectorExpression> vector_expression;
};

class ProjectionOperator final : public Operator {
  public:
    ProjectionOperator(std::unique_ptr<Operator> child, std::vector<ExecExprPtr> expressions, BufferPool* buffer_pool)
        : child_(std::move(child)), expressions_(std::move(expressions)), input_(buffer_pool, true) {}

    void Init() override;
    bool Next(VectorBatch& output) override;

  private:
    bool ProduceViewProjection(VectorBatch& output);
    bool ProduceVectorizedProjection(VectorBatch& output);
    bool ProduceMaterializedProjection(VectorBatch& output);

    std::unique_ptr<Operator> child_;
    std::vector<ExecExprPtr> expressions_;
    VectorBatch input_;

    // 每个输出列一个计划；下标与 expressions_ 一一对应。
    std::vector<ProjectionSlotPlan> slot_plans_;

    // 全部输出列都是直接列引用：走既有零拷贝 view 投影。
    bool all_direct_columns_ = false;
};

} // namespace simple_olap
