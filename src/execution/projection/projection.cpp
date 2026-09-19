#include "projection.h"

#include <algorithm>
#include <stdexcept>

#include "../batch_utils.h"
#include "../expression/vector_expression_compiler.h"
#include "../vector/gather_utils.h"

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

    // 收集所有 vector expression 依赖的输入列并去重：
    // 热路径不再分析表达式，只按 gather_slots_ 做 gather。
    gather_slots_.clear();
    for (const auto& plan : slot_plans_) {
        if (plan.kind == ProjectionSlotPlan::Kind::VECTOR_EXPRESSION) {
            plan.vector_expression->CollectInputSlots(gather_slots_);
        }
    }
    std::sort(gather_slots_.begin(), gather_slots_.end());
    gather_slots_.erase(std::unique(gather_slots_.begin(), gather_slots_.end()), gather_slots_.end());
}

bool ProjectionOperator::Next(VectorBatch& output) {
    while (child_->Next(input_)) {
        if (input_.ActiveSize() == 0) {
            continue;
        }

        // 全直接列引用：零拷贝视图 + selection 原样传播（dense / sparse 都走这里）。
        if (all_direct_columns_) {
            return ProduceViewProjection(output);
        }

        // 表达式投影：dense 直接 SIMD；sparse 先 typed gather 再 SIMD。
        if (input_.IsDense()) {
            return ProduceDenseProjection(output);
        }
        return ProduceSparseProjection(output);
    }

    ClearBatch(output);
    return false;
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

    // 不做任何物化：输出保持与输入相同的 selection（sparse 时仍是 sparse）。
    output.CopySelectionFrom(input_);
    return true;
}

bool ProjectionOperator::ProduceDenseProjection(VectorBatch& output) {
    ClearBatch(output);

    const uint32_t active = input_.ActiveSize();

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

    // 逐行物化的表达式：dense 下 physical row 即 dense row。
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

    output.SetIdentitySelection(active);
    return true;
}

bool ProjectionOperator::ProduceSparseProjection(VectorBatch& output) {
    ClearBatch(output);

    const uint32_t active = input_.ActiveSize();

    // 1. 建立所有输出列（保持顺序与 expressions_ 一致）。
    for (uint32_t col = 0; col < static_cast<uint32_t>(slot_plans_.size()); ++col) {
        const auto& plan = slot_plans_[col];
        switch (plan.kind) {
        case ProjectionSlotPlan::Kind::DIRECT_COLUMN: {
            if (plan.direct_slot >= input_.columns.size()) {
                throw std::runtime_error("ProjectionOperator: input slot out of range");
            }
            output.AddColumn(input_.columns[plan.direct_slot].type);
            break;
        }
        case ProjectionSlotPlan::Kind::VECTOR_EXPRESSION:
            output.AddColumn(plan.vector_expression->result_type());
            break;
        case ProjectionSlotPlan::Kind::SCALAR_EXPRESSION:
            output.AddColumn(expressions_[col]->GetReturnType());
            break;
        }
    }

    // 2. 直接列：typed gather 成 dense owned 列。
    //    （这些列若同时被 vector expression 依赖，第 3 步直接复用它们的 buffer。）
    BuildDirectOutputMap(static_cast<uint32_t>(input_.columns.size()));
    for (uint32_t col = 0; col < static_cast<uint32_t>(slot_plans_.size()); ++col) {
        const auto& plan = slot_plans_[col];
        if (plan.kind != ProjectionSlotPlan::Kind::DIRECT_COLUMN) {
            continue;
        }
        output.columns[col].Resize(active);
        GatherColumnDense(input_.columns[plan.direct_slot], input_.selection(), output.columns[col]);
    }

    // 3. 构造表达式专用的 dense 临时输入（只 gather 依赖列，且不重复 gather）。
    if (!gather_slots_.empty()) {
        BuildGatheredInput(input_, output, active);
    }

    // 4. 向量表达式：输入已经是 dense，继续复用现有 AVX2 内核。
    for (uint32_t col = 0; col < static_cast<uint32_t>(slot_plans_.size()); ++col) {
        const auto& plan = slot_plans_[col];
        if (plan.kind != ProjectionSlotPlan::Kind::VECTOR_EXPRESSION) {
            continue;
        }
        output.columns[col].Resize(active);
        plan.vector_expression->EvaluateDense(gathered_input_, output.columns[col], active);
    }

    // 5. 标量表达式：直接遍历 SelectionMask，physical row -> dense row。
    for (uint32_t col = 0; col < static_cast<uint32_t>(slot_plans_.size()); ++col) {
        if (slot_plans_[col].kind != ProjectionSlotPlan::Kind::SCALAR_EXPRESSION) {
            continue;
        }
        output.columns[col].Resize(active);
        const auto& expr = expressions_[col];
        uint32_t dense_row = 0;
        ForEachActiveRow(input_, [&](uint32_t physical_row) {
            const ExecValue value = expr->Eval(input_, physical_row);
            WriteExecValue(output.columns[col], dense_row, value);
            ++dense_row;
        });
    }

    // gather 后所有输出都是连续 active 行：重新成为 dense batch。
    output.SetIdentitySelection(active);
    return true;
}

void ProjectionOperator::BuildDirectOutputMap(uint32_t slot_count) {
    if (direct_output_of_slot_.size() < slot_count) {
        direct_output_of_slot_.resize(slot_count, kInvalidSlot);
    }
    std::fill(direct_output_of_slot_.begin(), direct_output_of_slot_.begin() + slot_count, kInvalidSlot);

    for (uint32_t col = 0; col < static_cast<uint32_t>(slot_plans_.size()); ++col) {
        const auto& plan = slot_plans_[col];
        if (plan.kind == ProjectionSlotPlan::Kind::DIRECT_COLUMN && plan.direct_slot < slot_count) {
            direct_output_of_slot_[plan.direct_slot] = col;
        }
    }
}

void ProjectionOperator::BuildGatheredInput(const VectorBatch& input, const VectorBatch& output, uint32_t active) {
    ClearBatch(gathered_input_);
    gathered_input_.columns.reserve(input.columns.size());

    for (uint32_t slot = 0; slot < static_cast<uint32_t>(input.columns.size()); ++slot) {
        const ColumnData& src = input.columns[slot];

        if (!std::binary_search(gather_slots_.begin(), gather_slots_.end(), slot)) {
            // 本列不参与任何 vector expression：占位，保持列下标与 input 对齐。
            // buffer 为 nullptr，表达式不会访问它。
            ColumnData placeholder(src.type, buffer_pool_);
            placeholder.count = active;
            gathered_input_.columns.push_back(std::move(placeholder));
            continue;
        }

        const uint32_t direct_col = slot < direct_output_of_slot_.size() ? direct_output_of_slot_[slot] : kInvalidSlot;
        if (direct_col != kInvalidSlot) {
            // 该列同时是直接投影输出：直接零拷贝复用已 gather 的输出 buffer。
            ColumnData view(src.type, buffer_pool_);
            view.CopyFrom(output.columns[direct_col].buffer, active, /*is_view=*/true);
            gathered_input_.columns.push_back(std::move(view));
        } else {
            gathered_input_.AddColumn(src.type);
            gathered_input_.columns.back().Resize(active);
            GatherColumnDense(src, input.selection(), gathered_input_.columns.back());
        }
    }

    gathered_input_.SetIdentitySelection(active);
}

} // namespace simple_olap
