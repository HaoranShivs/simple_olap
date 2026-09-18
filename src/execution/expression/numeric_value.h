#pragma once

#include <cstdint>
#include <type_traits>
#include <variant>

#include "../../type.h"
#include "exec_expression.h"

namespace simple_olap {

// ============================================================
// 数值字面量的「精确收敛」工具
// ============================================================
//
// SIMD 路径（VectorPredicate / VectorExpression）在 Init 阶段必须把 SQL 字面量
// 一次性收敛到列的存储类型，并且只允许「无损」收敛：
//   - 任何有损转换都返回 false，由调用方退回逐行标量路径；
//   - 于是 SIMD 结果与旧的逐行 long double 语义（CompareExecValues /
//     ExecValueAsNumber）在所有输入上都不可能分叉。
//
// 这里是唯一实现，供 vector_predicate_compiler 与 vector_expression_compiler 共用。

// 编译器已经绑定好类型的数值常量。
using NumericConstant = std::variant<int32_t, int64_t, float, double>;

inline bool IsNumericType(DataType type) {
    return type == DataType::INT32 || type == DataType::INT64 || type == DataType::FLOAT || type == DataType::DOUBLE;
}

// double -> int64：仅当 double 恰好是整数且能由 int64 精确表示时返回 true。
inline bool DoubleToInt64Exact(double v, int64_t& out) {
    if (v != v) { // NaN
        return false;
    }
    if (v < -9223372036854775808.0 || v >= 9223372036854775808.0) {
        return false;
    }
    if (v != static_cast<double>(static_cast<int64_t>(v))) {
        return false; // 含小数部分
    }
    out = static_cast<int64_t>(v);
    return true;
}

inline bool CoerceFromInt64(int64_t src, DataType target, NumericConstant& out) {
    switch (target) {
    case DataType::INT32:
        if (src >= INT32_MIN && src <= INT32_MAX) {
            out = static_cast<int32_t>(src);
            return true;
        }
        return false;
    case DataType::INT64:
        out = src;
        return true;
    case DataType::FLOAT:
        // float 可精确表示所有 |v| <= 2^24 的整数。
        if (src >= -16777216LL && src <= 16777216LL) {
            out = static_cast<float>(src);
            return true;
        }
        return false;
    case DataType::DOUBLE: {
        const double d = static_cast<double>(src);
        // 回读校验：只有能被 double 精确表示才走 SIMD，避免 > 2^53 精度损失。
        if (d >= -9223372036854775808.0 && d < 9223372036854775808.0 && static_cast<int64_t>(d) == src) {
            out = d;
            return true;
        }
        return false;
    }
    default:
        return false;
    }
}

inline bool CoerceFromDouble(double src, DataType target, NumericConstant& out) {
    switch (target) {
    case DataType::FLOAT: {
        const float f = static_cast<float>(src);
        if (static_cast<double>(f) == src) {
            out = f;
            return true;
        }
        return false;
    }
    case DataType::DOUBLE:
        out = src;
        return true;
    case DataType::INT32: {
        int64_t i = 0;
        if (!DoubleToInt64Exact(src, i) || i < INT32_MIN || i > INT32_MAX) {
            return false;
        }
        out = static_cast<int32_t>(i);
        return true;
    }
    case DataType::INT64: {
        int64_t i = 0;
        if (!DoubleToInt64Exact(src, i)) {
            return false;
        }
        out = i;
        return true;
    }
    default:
        return false;
    }
}

// 把 ExecValue 形式的字面量精确收敛到 target 类型；任何有损转换都返回 false。
inline bool TryCoerceExact(const ExecValue& value, DataType target, NumericConstant& out) {
    return std::visit(
        [&](const auto& v) -> bool {
            using T = std::decay_t<decltype(v)>;
            if constexpr (std::is_same_v<T, int32_t> || std::is_same_v<T, int64_t>) {
                return CoerceFromInt64(static_cast<int64_t>(v), target, out);
            } else if constexpr (std::is_same_v<T, float> || std::is_same_v<T, double>) {
                return CoerceFromDouble(static_cast<double>(v), target, out);
            } else {
                return false; // string / bool 不参与 SIMD 数值运算
            }
        },
        value);
}

} // namespace simple_olap
