# storage/segment

数据分段：按行范围切分的存储单元。

## 职责

- 一个 segment 包含固定行数（如 64K/128K 行）的数据，是并行扫描与 I/O 的基本单位
- 内部按列组织：每列对应一个 column_chunk
- 维护 segment 级统计信息（每列 min/max/null 计数），支持谓词下推时的 segment 级裁剪（zone map）
- 提供 segment 的序列化/反序列化（落盘与加载）

## 设计要点

- segment 是并行执行的天然切分粒度：不同 segment 可分配给不同线程
- 统计信息用于跳过不满足谓词的整个 segment，减少 I/O
- segment 内列数据独立存储，支持只读取所需列（列裁剪）