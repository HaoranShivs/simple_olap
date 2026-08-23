# execution/filter

过滤算子：谓词求值与行选择。

## 职责

- 对输入 `Batch` 求值 WHERE 谓词（向量化求值）
- 生成行选择（selection vector / 位图），标记满足条件的行
- 输出过滤后的 `Batch`（紧凑化或携带 selection 供下游使用）

## 设计要点

- 谓词向量化求值：整批计算，利用 SIMD 加速比较运算
- 支持复合谓词（AND/OR/NOT）的短路求值策略
- 与 scan 配合实现谓词下推：scan 先做 segment 级裁剪，filter 做行级过滤
- selection 可延迟物化（late materialization）：先过滤再取列，减少解码量