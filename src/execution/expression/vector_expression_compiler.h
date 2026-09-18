#pragma once

#include <memory>

#include "vector_expression.h"

namespace simple_olap {

// 把执行期表达式树（ExecExpression）编译成 VectorExpression。
//
// 编译在 ProjectionOperator::Init() 中只做一次，热路径只执行已绑定好的
// slot / 类型 / 内核，不再遍历表达式树、不再构造 variant。
//
// 返回 nullptr 表示「无法保证与逐行语义等价」，由调用方退回逐行物化：
//   - 第一版只支持 ADD / SUB（无乘除）；
//   - 操作数只能是「同类型的列引用」或「可精确收敛到该类型的数值字面量」；
//   - 其余情况（VARCHAR、混合数值类型、嵌套算术、常量 OP 常量等）返回 nullptr。
std::unique_ptr<VectorExpression> TryCompileVectorExpression(const ExecExpression& expr);

} // namespace simple_olap
