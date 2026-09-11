#pragma once

#include <cstdint>

namespace simple_olap {

// 单个 VectorBatch 的行数上限（执行期批处理粒度）。
//
// 放在 common 层而不是 execution/vector 层，目的是让 simd 层只依赖
// DataType / CmpOp / 指针 / count / mask，而不需要 include vector.h。
// 这样依赖方向保持为：type/common <- simd <- execution/storage。
inline constexpr uint32_t kVectorBatchSize = 1024;

} // namespace simple_olap
