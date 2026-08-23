# test

单元测试与集成测试。

## 职责

- 单元测试：覆盖各模块核心逻辑
  - lexer/parser：token 切分、AST 构建、错误报告
  - optimizer：谓词下推、列裁剪等规则的正确性
  - encoding：各编码的 encode/decode 往返一致性
  - execution：各算子的批处理正确性（含 null 语义）
  - arena：分配/释放、对齐、容量
- 集成测试：端到端 SQL 查询
  - 建表 → 写入 → 查询（SELECT/WHERE/GROUP BY）→ 校验结果
  - 跨模块协作：planner + storage + execution 全链路
- 测试数据：小型固定数据集，保证可复现

## 设计要点

- 测试与 benchmark 分离：test 验证正确性，benchmark 验证性能
- 集成测试以 SQL 为入口，断言查询结果
- 边界用例：空表、全 null 列、单行、超大 batch