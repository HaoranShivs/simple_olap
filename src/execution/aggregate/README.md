# execution/aggregate

聚合算子：GROUP BY 与聚合函数。

## 职责

- 实现聚合函数：`COUNT`、`SUM`、`MIN`、`MAX`、`AVG`（向量化累加）
- 实现 GROUP BY：
  - 哈希聚合（HashAggregate）：哈希表分组，适合高基数
  - 排序聚合（SortAggregate）：先排序再归并，适合低基数/已排序数据
- 支持多列分组键与 HAVING 过滤

## 设计要点

- 聚合状态（partial state）按批增量更新，避免一次性物化全表
- 并行聚合：各线程维护局部哈希表，最后合并（two-phase aggregation）
- 分组键哈希需处理 null 语义（null 值归入同一组或按 SQL 语义处理）