#include "vector_expression.h"

#include <stdexcept>

#include "../../simd/kernels.h"

namespace simple_olap {

namespace {

// 取列的 typed 起始指针；slot 越界视为内部错误（编译器只生成已验证的 slot）。
template <typename T> const T* ColumnDataPtr(const VectorBatch& batch, uint32_t slot) {
    if (slot >= batch.columns.size()) {
        throw std::runtime_error("ArithmeticVectorExpression: input slot out of range");
    }
    return batch.columns[slot].data<T>();
}

} // namespace

void ArithmeticVectorExpression::EvaluateDense(const VectorBatch& input, ColumnData& output, uint32_t count) const {
    if (count == 0) {
        return;
    }
    if (output.type != type_) {
        throw std::runtime_error("ArithmeticVectorExpression: output type mismatch");
    }

    const auto& kernels = simd::KernelRegistry::Instance().arithmetic();

    const bool lhs_is_column = lhs_.kind == ArithmeticOperand::Kind::COLUMN;
    const bool rhs_is_column = rhs_.kind == ArithmeticOperand::Kind::COLUMN;

    // ---------- Column OP Column ----------
    if (lhs_is_column && rhs_is_column) {
        switch (type_) {
        case DataType::INT32:
            kernels.i32_column(op_, ColumnDataPtr<int32_t>(input, lhs_.column_slot),
                               ColumnDataPtr<int32_t>(input, rhs_.column_slot), output.mutable_data<int32_t>(), count);
            return;
        case DataType::INT64:
            kernels.i64_column(op_, ColumnDataPtr<int64_t>(input, lhs_.column_slot),
                               ColumnDataPtr<int64_t>(input, rhs_.column_slot), output.mutable_data<int64_t>(), count);
            return;
        case DataType::FLOAT:
            kernels.f32_column(op_, ColumnDataPtr<float>(input, lhs_.column_slot),
                               ColumnDataPtr<float>(input, rhs_.column_slot), output.mutable_data<float>(), count);
            return;
        case DataType::DOUBLE:
            kernels.f64_column(op_, ColumnDataPtr<double>(input, lhs_.column_slot),
                               ColumnDataPtr<double>(input, rhs_.column_slot), output.mutable_data<double>(), count);
            return;
        default:
            throw std::runtime_error("ArithmeticVectorExpression: non-numeric result type");
        }
    }

    // ---------- Column OP Constant / Constant OP Column ----------
    const ArithmeticOperand* column_operand = lhs_is_column ? &lhs_ : &rhs_;
    const ArithmeticOperand* constant_operand = lhs_is_column ? &rhs_ : &lhs_;
    const bool const_on_left = !lhs_is_column;

    switch (type_) {
    case DataType::INT32:
        kernels.i32_const(op_, ColumnDataPtr<int32_t>(input, column_operand->column_slot),
                          std::get<int32_t>(constant_operand->constant), const_on_left, output.mutable_data<int32_t>(),
                          count);
        return;
    case DataType::INT64:
        kernels.i64_const(op_, ColumnDataPtr<int64_t>(input, column_operand->column_slot),
                          std::get<int64_t>(constant_operand->constant), const_on_left, output.mutable_data<int64_t>(),
                          count);
        return;
    case DataType::FLOAT:
        kernels.f32_const(op_, ColumnDataPtr<float>(input, column_operand->column_slot),
                          std::get<float>(constant_operand->constant), const_on_left, output.mutable_data<float>(),
                          count);
        return;
    case DataType::DOUBLE:
        kernels.f64_const(op_, ColumnDataPtr<double>(input, column_operand->column_slot),
                          std::get<double>(constant_operand->constant), const_on_left, output.mutable_data<double>(),
                          count);
        return;
    default:
        throw std::runtime_error("ArithmeticVectorExpression: non-numeric result type");
    }
}

} // namespace simple_olap
