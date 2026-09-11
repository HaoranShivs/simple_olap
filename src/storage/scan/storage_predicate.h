#pragma once

#include <cstdint>
#include <variant>
#include <vector>

#include "../../type.h"
#include "../datastructs.h"

namespace simple_olap {

// 一个“已准备好、可直接喂给 compare kernel”的存储谓词。
//
// Build 阶段一次性完成：
//   - ColumnId -> output.columns 下标（output_slot）
//   - literal 按列类型做 typed binding（variant<int32_t,int64_t,double,string>
//     -> variant<int32_t,int64_t,float,double>）
//
// 热路径（StorageRowFilter）因此不再执行 std::find，也不再解析 variant。
struct PreparedStoragePredicate {
    enum class Kind : uint8_t {
        // 正常类型化比较：走 SIMD/scalar compare kernel。
        TYPED_COMPARE = 0,
        // 该谓词对任意行恒成立（如 <= 常量落在范围外）：扫描时整条跳过。
        ALWAYS_TRUE,
        // 该谓词对任意行恒不成立（如整数列 = 非整数常量）：整批全 false。
        ALWAYS_FALSE,
    };

    Kind kind = Kind::TYPED_COMPARE;

    // predicate 列在 output.columns 中的下标（不是 ColumnId）
    uint32_t output_slot = 0;

    DataType type = DataType::INVALID;
    CmpOp op = CmpOp::EQ;

    // 与 type 对应的类型化字面量：INT32->int32_t, INT64->int64_t,
    // FLOAT->float, DOUBLE->double。
    std::variant<int32_t, int64_t, float, double> value;
};

// ScanOptions 中所有 pushed predicate 的不可变准备结果。
// 串行 ScanCursor 与并行 scan worker 都只读共享同一份实例。
class PreparedScanPredicates {
  public:
    // 一次性准备：
    //   - 校验 predicate 列已在 options.columns 中（否则抛内部错误，与旧行为一致）
    //   - 按 schema 找到列类型并完成 typed literal binding
    //
    // 返回结果与 options.predicates 严格一一对应（含 ALWAYS_TRUE / ALWAYS_FALSE），
    // 因此 row_filter_mask[p] 的下标语义保持不变。
    static PreparedScanPredicates Build(const ScanOptions& options, const TableSchema& schema);

    const std::vector<PreparedStoragePredicate>& predicates() const noexcept {
        return predicates_;
    }

    bool empty() const noexcept {
        return predicates_.empty();
    }

    size_t size() const noexcept {
        return predicates_.size();
    }

  private:
    std::vector<PreparedStoragePredicate> predicates_;
};

} // namespace simple_olap
