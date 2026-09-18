#include "projection.h"

#include <stdexcept>

#include "../batch_utils.h"
#include "../expression/vector_expression_compiler.h"

namespace simple_olap {

void ProjectionOperator::Init() {
    child_->Init();
    ClearBatch(input_);

    // 每个输出列只分类一次：直接列引用 / 已编译整列表达式 / 逐行标量。
    slot_plans_.clear();
    slot_plans_.reserve(expressions_.size());
    all_direct_columns_ = true;

    for (const auto& expr : expressions_) {
        ProjectionSlotPlan plan;

        if (expr->GetType() == ExecExpression::Type::COLUMN_REF) {
            const auto& ref = static_cast<const ExecColumnRef&>(*expr);
            plan.kind = ProjectionSlotPlan::Kind::DIRECT_COLUMN;
            plan.direct_slot = ref.GetInputSlot();
        } else {
            all_direct_columns_ = false;
            plan.vector_expression = TryCompileVectorExpression(*expr);
            plan.kind = plan.vector_expression != nullptr ? ProjectionSlotPlan::Kind::VECTOR_EXPRESSION
                                                          : ProjectionSlotPlan::Kind::SCALAR_EXPRESSION;
        }

        slot_plans_.push_back(std::move(plan));
    }
}

bool ProjectionOperator::Next(VectorBatch& output) {
    if (!child_->Next(input_)) {
        ClearBatch(output);
        return false;
    }

    // 全直接列引用：零拷贝 view 投影（既有行为，保持不变）。
    if (all_direct_columns_) {
        return ProduceViewProjection(output);
    }

    // 混合/表达式投影：dense 输入走整列（SIMD）求值；
    // selection 非空时退回逐行物化（第一版不做 gather）。
    if (input_.sel_vector.empty()) {
        return ProduceVectorizedProjection(output);
    }
    return ProduceMaterializedProjection(output);
}

// ---------- 内部实现 ----------

bool ProjectionOperator::ProduceViewProjection(VectorBatch& output) {
    ClearBatch(output);

    for (const auto& expr : expressions_) {
        const auto& ref = static_cast<const ExecColumnRef&>(*expr);
        const uint32_t slot = ref.GetInputSlot();
        if (slot >= input_.columns.size()) {
            throw std::runtime_error("ProjectionOperator: input slot out of range");
        }

        const auto& src = input_.columns[slot];
        output.AddColumn(src.type);
        output.columns.back().CopyFrom(src.buffer, src.count, true);
    }

    output.size = ActiveRowCount(input_);
    output.sel_vector = input_.sel_vector;
    return true;
}

bool ProjectionOperator::ProduceVectorizedProjection(VectorBatch& output) {
    ClearBatch(output);

    // 前置条件：input_ 为 dense（physical == active），否则调用方会走物化路径。
    const uint32_t active = ActiveRowCount(input_);

    // 先建立所有输出列（保持列顺序与 slot_plans_ 一致），再逐列填值。
    for (uint32_t col = 0; col < static_cast<uint32_t>(slot_plans_.size()); ++col) {
        const auto& plan = slot_plans_[col];
        switch (plan.kind) {
        case ProjectionSlotPlan::Kind::DIRECT_COLUMN: {
            if (plan.direct_slot >= input_.columns.size()) {
                throw std::runtime_error("ProjectionOperator: input slot out of range");
            }
            const auto& src = input_.columns[plan.direct_slot];
            output.AddColumn(src.type);
            output.columns.back().CopyFrom(src.buffer, src.count, true); // 零拷贝视图
            break;
        }
        case ProjectionSlotPlan::Kind::VECTOR_EXPRESSION: {
            const DataType type = plan.vector_expression->result_type();
            output.AddColumn(type);
            output.columns.back().Resize(active);
            plan.vector_expression->EvaluateDense(input_, output.columns.back(), active);
            break;
        }
        case ProjectionSlotPlan::Kind::SCALAR_EXPRESSION:
            output.AddColumn(expressions_[col]->GetReturnType());
            output.columns.back().Resize(active);
            break;
        }
    }

    // 逐行物化的表达式：dense 下 physical row 即 logical row。
    for (uint32_t col = 0; col < static_cast<uint32_t>(slot_plans_.size()); ++col) {
        if (slot_plans_[col].kind != ProjectionSlotPlan::Kind::SCALAR_EXPRESSION) {
            continue;
        }
        const auto& expr = expressions_[col];
        for (uint32_t row = 0; row < active; ++row) {
            const ExecValue value = expr->Eval(input_, row);
            WriteExecValue(output.columns[col], row, value);
        }
    }

    output.size = active;
    output.sel_vector.clear();
    return true;
}

bool ProjectionOperator::ProduceMaterializedProjection(VectorBatch& output) {
    ClearBatch(output);

    const uint32_t active = ActiveRowCount(input_);
    for (const auto& expr : expressions_) {
        output.AddColumn(expr->GetReturnType());
        output.columns.back().Resize(active);
    }

    for (uint32_t logical = 0; logical < active; ++logical) {
        const uint32_t physical = ActiveRowIndex(input_, logical);
        for (uint32_t col = 0; col < static_cast<uint32_t>(expressions_.size()); ++col) {
            const ExecValue value = expressions_[col]->Eval(input_, physical);
            WriteExecValue(output.columns[col], logical, value);
        }
    }

    output.size = active;
    output.sel_vector.clear();
    return true;
}

} // namespace simple_olap
