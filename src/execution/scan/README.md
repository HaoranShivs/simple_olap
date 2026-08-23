# execution/scan

扫描算子：从列式存储读取数据。

## 职责

- 按物理计划指定的表与列，从 storage 读取数据并产出 `Batch`
- 应用 segment 级裁剪：利用 zone map（min/max 统计）跳过不满足谓词的 segment
- 列裁剪：只读取计划需要的列
- 支持并行：不同 segment 分配给不同线程扫描

## 设计要点

- 实现统一的算子迭代器接口：`open / get_next(batch) / close`
- 扫描是 I/O 与解码的瓶颈，需与 encoding 解码、file 读取紧密配合
- 输出 Batch 的列顺序与 schema 对齐，供下游 filter/projection 使用