#include "storage_predicate.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace simple_olap {
namespace {

using Kind = PreparedStoragePredicate::Kind;

[[noreturn]] void ThrowUnsupportedLiteral() {
    throw std::runtime_error("pushed predicate contains unsupported literal type");
}

[[noreturn]] void ThrowUnsupportedColumn() {
    throw std::runtime_error("pushed predicate contains unsupported column type");
}

void SetResult(Kind kind, PreparedStoragePredicate& p) {
    p.kind = kind;
}

// 在“整数常量位于整数列类型范围外”时，谓词结果恒定。
// value 与 [lo, hi] 都是整数域比较。
void SetIntegerConstantPredicate(int64_t lo, int64_t hi, CmpOp op, int64_t value, PreparedStoragePredicate& p) {
    if (value < lo || value > hi) {
        const bool below = (value < lo);
        switch (op) {
        case CmpOp::EQ:
            SetResult(Kind::ALWAYS_FALSE, p);
            return;
        case CmpOp::NE:
            SetResult(Kind::ALWAYS_TRUE, p);
            return;
        case CmpOp::GT:
        case CmpOp::GE:
            // value < lo: 所有列值都 > value；value > hi: 所有列值都 <= value
            SetResult(below ? Kind::ALWAYS_TRUE : Kind::ALWAYS_FALSE, p);
            return;
        case CmpOp::LT:
        case CmpOp::LE:
            SetResult(below ? Kind::ALWAYS_FALSE : Kind::ALWAYS_TRUE, p);
            return;
        }
        return;
    }

    p.kind = Kind::TYPED_COMPARE;
    p.op = op;
    if (p.type == DataType::INT32) {
        p.value = static_cast<int32_t>(value);
    } else {
        p.value = value;
    }
}

// 整数列 + double 字面量。
// lo/hi 为列类型的整数范围；lo_d/hi_excl_d 为对应的 double 边界（hi 为开区间）。
void SetIntegerDoublePredicate(int64_t lo, int64_t hi, double lo_d, double hi_excl_d, CmpOp op, double d,
                               PreparedStoragePredicate& p) {
    // 超出列类型范围（或 NaN）：结果恒定，不能安全地 cast。
    if (!(d >= lo_d && d < hi_excl_d)) {
        if (std::isnan(d)) {
            SetResult(op == CmpOp::NE ? Kind::ALWAYS_TRUE : Kind::ALWAYS_FALSE, p);
            return;
        }
        const bool too_big = (d >= hi_excl_d);
        switch (op) {
        case CmpOp::EQ:
            SetResult(Kind::ALWAYS_FALSE, p);
            return;
        case CmpOp::NE:
            SetResult(Kind::ALWAYS_TRUE, p);
            return;
        case CmpOp::GT:
        case CmpOp::GE:
            SetResult(too_big ? Kind::ALWAYS_FALSE : Kind::ALWAYS_TRUE, p);
            return;
        case CmpOp::LT:
        case CmpOp::LE:
            SetResult(too_big ? Kind::ALWAYS_TRUE : Kind::ALWAYS_FALSE, p);
            return;
        }
        return;
    }

    if (std::floor(d) == d) {
        SetIntegerConstantPredicate(lo, hi, op, static_cast<int64_t>(d), p);
        return;
    }

    // 非整数常量：改写成等价的整数比较，避免截断改变语义。
    //   col >  d  <=> col >  floor(d)
    //   col >= d  <=> col >  floor(d)
    //   col <  d  <=> col <  ceil(d)
    //   col <= d  <=> col <  ceil(d)
    //   col == d  <=> 恒 false
    //   col != d  <=> 恒 true
    switch (op) {
    case CmpOp::EQ:
        SetResult(Kind::ALWAYS_FALSE, p);
        return;
    case CmpOp::NE:
        SetResult(Kind::ALWAYS_TRUE, p);
        return;
    case CmpOp::GT:
    case CmpOp::GE:
        SetIntegerConstantPredicate(lo, hi, CmpOp::GT, static_cast<int64_t>(std::floor(d)), p);
        return;
    case CmpOp::LT:
    case CmpOp::LE:
        SetIntegerConstantPredicate(lo, hi, CmpOp::LT, static_cast<int64_t>(std::ceil(d)), p);
        return;
    }
}

bool LiteralAsDouble(const Condition& cond, double& out) {
    if (const auto* v32 = std::get_if<int32_t>(&cond.value)) {
        out = static_cast<double>(*v32);
        return true;
    }
    if (const auto* v64 = std::get_if<int64_t>(&cond.value)) {
        out = static_cast<double>(*v64);
        return true;
    }
    if (const auto* vd = std::get_if<double>(&cond.value)) {
        out = *vd;
        return true;
    }
    return false; // std::string
}

void BindIntegerPredicate(DataType type, CmpOp op, const Condition& cond, PreparedStoragePredicate& p) {
    int64_t lo = 0;
    int64_t hi = 0;
    double lo_d = 0.0;
    double hi_excl_d = 0.0;

    if (type == DataType::INT32) {
        lo = -2147483648LL;
        hi = 2147483647LL;
        lo_d = -2147483648.0;
        hi_excl_d = 2147483648.0;
    } else {
        lo = INT64_MIN;
        hi = INT64_MAX;
        lo_d = -9223372036854775808.0;     // -2^63，可精确表示
        hi_excl_d = 9223372036854775808.0; // 2^63，开区间上界
    }

    if (const auto* v32 = std::get_if<int32_t>(&cond.value)) {
        SetIntegerConstantPredicate(lo, hi, op, static_cast<int64_t>(*v32), p);
        return;
    }
    if (const auto* v64 = std::get_if<int64_t>(&cond.value)) {
        SetIntegerConstantPredicate(lo, hi, op, *v64, p);
        return;
    }
    if (const auto* vd = std::get_if<double>(&cond.value)) {
        SetIntegerDoublePredicate(lo, hi, lo_d, hi_excl_d, op, *vd, p);
        return;
    }
    ThrowUnsupportedLiteral();
}

void BindFloatingPredicate(DataType type, CmpOp op, const Condition& cond, PreparedStoragePredicate& p) {
    double v = 0.0;
    if (!LiteralAsDouble(cond, v)) {
        ThrowUnsupportedLiteral();
    }

    p.kind = Kind::TYPED_COMPARE;
    p.op = op;
    if (type == DataType::FLOAT) {
        p.value = static_cast<float>(v);
    } else {
        p.value = v;
    }
}

} // namespace

PreparedScanPredicates PreparedScanPredicates::Build(const ScanOptions& options, const TableSchema& schema) {
    PreparedScanPredicates result;
    result.predicates_.reserve(options.predicates.size());

    for (const Condition& cond : options.predicates) {
        // ColumnId -> output.columns 下标（只做一次，避免每 batch std::find）
        const auto it = std::find(options.columns.begin(), options.columns.end(), cond.column);
        if (it == options.columns.end()) {
            throw std::runtime_error("predicate column is not scanned");
        }
        const uint32_t slot = static_cast<uint32_t>(it - options.columns.begin());

        DataType column_type = DataType::INVALID;
        for (const ColumnSchema& column : schema.columns) {
            if (column.column_id == cond.column) {
                column_type = column.type;
                break;
            }
        }
        if (column_type == DataType::INVALID) {
            throw std::runtime_error("pushed predicate references unknown column");
        }

        PreparedStoragePredicate p;
        p.output_slot = slot;
        p.type = column_type;
        p.op = cond.op;

        switch (column_type) {
        case DataType::INT32:
        case DataType::INT64:
            BindIntegerPredicate(column_type, cond.op, cond, p);
            break;
        case DataType::FLOAT:
        case DataType::DOUBLE:
            BindFloatingPredicate(column_type, cond.op, cond, p);
            break;
        default:
            ThrowUnsupportedColumn();
        }

        result.predicates_.push_back(std::move(p));
    }

    return result;
}

} // namespace simple_olap
