# planner/physical_plan

物理计划：将优化后的逻辑计划映射为可执行的物理算子。

## 职责

- 为每个逻辑算子选择具体的物理实现（如 `HashAggregate` vs `SortAggregate`）
- 确定并行度、数据分片方式（segment 级切分）
- 生成执行引擎可直接调度的物理算子树

## 设计要点

- 物理算子接口统一：`open / get_next(batch) / close` 的迭代器模型
- 算子间以列式批（vector/Batch）传递数据
- 物理计划需携带执行上下文（内存 arena、线程池、存储句柄）