# storage/file

文件格式：列式数据的磁盘布局与读写。

## 职责

- 定义列式文件格式（参考 Parquet/ORC 简化版）：
  - 文件头：magic、版本、schema
  - 数据区：按 segment → column_chunk 顺序排列的编码数据
  - 文件尾：footer（segment 偏移索引、统计信息、schema）
- 提供文件的写入器（Writer）与读取器（Reader）
- 支持按 column_chunk 定位读取（随机访问单列单段）

## 设计要点

- footer 索引使读取时无需扫描整个文件即可定位目标列块
- 文件格式需版本化，预留扩展字段
- 写入采用"内存 segment 满后批量落盘"策略，减少小文件