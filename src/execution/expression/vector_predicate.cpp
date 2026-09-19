#include "vector_predicate.h"

#include <stdexcept>

#include "../../simd/kernels.h"

namespace simple_olap {

namespace {

// 把 kernel 结果限制到 input_mask 上，维持 output ⊆ input 不变式。
inline void IntersectWithInput(simd::SelectionMask& output, const simd::SelectionMask& input) {
    output.And(input);
}

// 取列的 typed 起始指针；slot 越界时返回 nullptr（由调用方转为空结果）。
const void* ColumnBuffer(const VectorBatch& batch, uint32_t slot) {
    if (slot >= batch.columns.size()) {
        return nullptr;
    }
    return batch.columns[slot].buffer;
}

} // namespace

void NumericComparePredicate::Evaluate(const VectorBatch& batch, uint32_t physical_count,
                                       const simd::SelectionMask& input_mask, simd::SelectionMask& output_mask) const {
    output_mask.SetNone(physical_count);
    if (input_mask.Empty()) {
        return;
    }

    const auto& kernels = simd::KernelRegistry::Instance().compare();

    // ---------- Column OP Column ----------
    if (lhs_.kind == CompareOperand::Kind::COLUMN && rhs_.kind == CompareOperand::Kind::COLUMN) {
        const void* lhs_ptr = ColumnBuffer(batch, lhs_.column_slot);
        const void* rhs_ptr = ColumnBuffer(batch, rhs_.column_slot);
        if (lhs_ptr == nullptr || rhs_ptr == nullptr) {
            return;
        }

        switch (type_) {
        case DataType::INT32:
            kernels.i32_column(static_cast<const int32_t*>(lhs_ptr), static_cast<const int32_t*>(rhs_ptr),
                               physical_count, op_, output_mask);
            break;
        case DataType::INT64:
            kernels.i64_column(static_cast<const int64_t*>(lhs_ptr), static_cast<const int64_t*>(rhs_ptr),
                               physical_count, op_, output_mask);
            break;
        case DataType::FLOAT:
            kernels.f32_column(static_cast<const float*>(lhs_ptr), static_cast<const float*>(rhs_ptr), physical_count,
                               op_, output_mask);
            break;
        case DataType::DOUBLE:
            kernels.f64_column(static_cast<const double*>(lhs_ptr), static_cast<const double*>(rhs_ptr), physical_count,
                               op_, output_mask);
            break;
        default:
            return;
        }

        IntersectWithInput(output_mask, input_mask);
        return;
    }

    // ---------- Column OP Constant（编译器保证 column 在左） ----------
    const CompareOperand* column_operand = nullptr;
    const CompareOperand* constant_operand = nullptr;
    if (lhs_.kind == CompareOperand::Kind::COLUMN && rhs_.kind == CompareOperand::Kind::CONSTANT) {
        column_operand = &lhs_;
        constant_operand = &rhs_;
    } else if (lhs_.kind == CompareOperand::Kind::CONSTANT && rhs_.kind == CompareOperand::Kind::COLUMN) {
        column_operand = &rhs_;
        constant_operand = &lhs_;
    }
    if (column_operand == nullptr || constant_operand == nullptr) {
        return;
    }

    const void* data = ColumnBuffer(batch, column_operand->column_slot);
    if (data == nullptr) {
        return;
    }

    switch (type_) {
    case DataType::INT32:
        kernels.i32_const(static_cast<const int32_t*>(data), physical_count, op_,
                          std::get<int32_t>(constant_operand->constant), output_mask);
        break;
    case DataType::INT64:
        kernels.i64_const(static_cast<const int64_t*>(data), physical_count, op_,
                          std::get<int64_t>(constant_operand->constant), output_mask);
        break;
    case DataType::FLOAT:
        kernels.f32_const(static_cast<const float*>(data), physical_count, op_,
                          std::get<float>(constant_operand->constant), output_mask);
        break;
    case DataType::DOUBLE:
        kernels.f64_const(static_cast<const double*>(data), physical_count, op_,
                          std::get<double>(constant_operand->constant), output_mask);
        break;
    default:
        return;
    }

    IntersectWithInput(output_mask, input_mask);
}

void LogicalVectorPredicate::Evaluate(const VectorBatch& batch, uint32_t physical_count,
                                      const simd::SelectionMask& input_mask, simd::SelectionMask& output_mask) const {
    output_mask.SetNone(physical_count);
    if (input_mask.Empty()) {
        return;
    }

    lhs_->Evaluate(batch, physical_count, input_mask, output_mask);

    if (op_ == Op::AND) {
        // 左侧已经全假：右侧不可能再翻盘。
        if (output_mask.Empty()) {
            return;
        }

        simd::SelectionMask rhs_mask;
        rhs_mask.SetNone(physical_count);
        rhs_->Evaluate(batch, physical_count, output_mask, rhs_mask);
        output_mask.And(rhs_mask);
        return;
    }

    // OR：若左侧已经覆盖全部输入行，则无需计算右侧。
    if (output_mask.Count() >= input_mask.Count()) {
        return;
    }

    // 右侧只在「输入为真且左侧为假」的行上求值。
    simd::SelectionMask rhs_input = input_mask;
    rhs_input.AndNot(output_mask);

    simd::SelectionMask rhs_mask;
    rhs_mask.SetNone(physical_count);
    rhs_->Evaluate(batch, physical_count, rhs_input, rhs_mask);
    output_mask.Or(rhs_mask);
}

void ScalarVectorPredicate::Evaluate(const VectorBatch& batch, uint32_t physical_count,
                                     const simd::SelectionMask& input_mask, simd::SelectionMask& output_mask) const {
    output_mask.SetNone(physical_count);
    if (input_mask.Empty()) {
        return;
    }

    uint64_t* words = output_mask.data();
    // 只遍历输入 mask 的置位行：1024 行里只剩 30 行时，只做 30 次 Eval，
    // 不再对全部物理行做 Test + continue。
    input_mask.ForEachSetBit([&](uint32_t row) {
        if (ExecValueAsBool(expr_->Eval(batch, row))) {
            words[row >> 6] |= (uint64_t(1) << (row & 63));
        }
    });
}

} // namespace simple_olap
