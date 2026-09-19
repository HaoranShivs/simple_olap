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
        VECTOR_EXPRESSION, // 已编译成整列求值：dense/gather 后走 SIMD/向量内核
        SCALAR_EXPRESSION, // 其余情况：逐行 Eval 物化
    };

    Kind kind = Kind::SCALAR_EXPRESSION;

    // kind == DIRECT_COLUMN 时的输入列下标
    uint32_t direct_slot = 0;

    // kind == VECTOR_EXPRESSION 时的已编译表达式（拥有所有权）
    std::unique_ptr<VectorExpression> vector_expression;
};

// ProjectionOperator：把子算子 batch 投影成输出列。
//
// 优化原则：Sparse != Scalar。优化粒度保持在 column/slot，而不是整个算子。
//
// 三条输出路径：
//
//   ViewProjection（全直接列）
//       zero-copy 切列 + selection 原样传播（late materialization）
//
//   DenseProjection（input dense）
//       直接列：零拷贝视图
//       向量表达式：现有 AVX2 arithmetic kernel
//       标量表达式：连续 for 逐行 Eval
//
//   SparseProjection（input sparse）
//       SelectionMask -> typed gather -> dense 临时 batch -> 现有 AVX2 kernel
//       标量表达式：直接遍历 SelectionMask，不生成 selection vector
class ProjectionOperator final : public Operator {
  public:
    ProjectionOperator(std::unique_ptr<Operator> child, std::vector<ExecExprPtr> expressions, BufferPool* buffer_pool)
        : child_(std::move(child)), expressions_(std::move(expressions)), input_(buffer_pool, true),
          gathered_input_(buffer_pool, false), buffer_pool_(buffer_pool) {}

    void Init() override;
    bool Next(VectorBatch& output) override;

  private:
    bool ProduceViewProjection(VectorBatch& output);
    bool ProduceDenseProjection(VectorBatch& output);
    bool ProduceSparseProjection(VectorBatch& output);

    // sparse 路径：把 vector expression 依赖的列 gather 成 dense 临时 batch。
    // 若某个依赖列同时是直接投影输出，直接复用其已 gather 的 buffer（不重复 gather）。
    void BuildGatheredInput(const VectorBatch& input, const VectorBatch& output, uint32_t active);

    // 建立 input slot -> 直接投影输出列 的映射（无对应列时为 kInvalidSlot）。
    void BuildDirectOutputMap(uint32_t slot_count);

    static constexpr uint32_t kInvalidSlot = 0xFFFFFFFFu;

    std::unique_ptr<Operator> child_;
    std::vector<ExecExprPtr> expressions_;
    VectorBatch input_;

    // sparse 路径的 dense 临时输入（只物化表达式依赖的列）。
    VectorBatch gathered_input_;

    // 每个输出列一个计划；下标与 expressions_ 一一对应。
    std::vector<ProjectionSlotPlan> slot_plans_;

    // 全部输出列都是直接列引用：走零拷贝 view 投影。
    bool all_direct_columns_ = false;

    // 所有 vector expression 依赖的输入 slot（去重、升序）。
    std::vector<uint32_t> gather_slots_;

    // input slot -> 直接投影输出列下标（kInvalidSlot 表示没有直接投影）。
    std::vector<uint32_t> direct_output_of_slot_;

    BufferPool* buffer_pool_ = nullptr;
};

} // namespace simple_olap
