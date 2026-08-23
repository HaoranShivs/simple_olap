# planner/logical_plan

逻辑计划：以关系代数算子树表示查询语义。

## 职责

- 将 AST 翻译为逻辑算子树：`Scan`、`Filter`、`Project`、`Aggregate`、`Sort`、`Limit` 等
- 算子只表达"做什么"，不关心"怎么做"（无物理实现细节）
- 提供计划树的遍历、重写（rule-based rewrite）基础设施

## 设计要点

- 逻辑算子与物理算子一一对应但相互独立
- 谓词下推、列裁剪等优化以逻辑计划变换的形式表达
- 计划节点需可序列化/可打印，便于调试与 EXPLAIN 输出