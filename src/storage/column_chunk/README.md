# storage/column_chunk

列块：segment 内单列的连续存储。

## 职责

- 存储一个 segment 中某一列的全部值（定长或变长）
- 维护该列的 null 位图（validity bitmap）
- 提供按行范围读取列数据的能力（供 scan 算子使用）
- 记录列的编码方式与压缩后大小

## 设计要点

- 列块是编码/压缩（encoding）的作用对象
- 定长列（int/float）与变长列（string）采用不同的内存布局
- 读取时按需解码，支持只解码谓词命中的行（late materialization 可选）