#pragma once

#include <memory>

#include "vector_predicate.h"

namespace simple_olap {

// 把执行期表达式树（ExecExpression）编译成 VectorPredicate 树。
//
// 编译在 FilterOperator::Init() 中只做一次，热路径只执行已绑定好的
// slot / 类型 / 内核，不再遍历表达式树、不再构造 variant。
//
// 本函数永不返回 nullptr：
//   - 可 SIMD 的比较（Column OP Constant / Column OP Column，类型精确收敛）
//     编译为 NumericComparePredicate；
//   - AND / OR 递归编译为 LogicalVectorPredicate；
//   - 其余任意子树（VARCHAR、混合数值类型、嵌套算术等）退化为
//     ScalarVectorPredicate，逐行语义与旧 Filter 完全一致。
std::unique_ptr<VectorPredicate> CompileVectorPredicate(const ExecExpression& expr);

} // namespace simple_olap
