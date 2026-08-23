# parallel/thread_pool

线程池：并行执行调度。

## 职责

- 管理固定大小的工作线程池
- 提供任务提交接口：`submit(closure) -> Future`
- 支持"map 到 segment 切片"的并行模式：将表的 segment 列表切分给各线程
- 提供任务结果收集与错误传播

## 设计要点

- 线程数默认等于 CPU 核心数，可配置
- 任务粒度以 segment 为单位，避免过细调度开销
- 支持两阶段并行聚合：各线程局部聚合 + 主线程合并
- 工作窃取（work-stealing）可选，应对 segment 大小不均