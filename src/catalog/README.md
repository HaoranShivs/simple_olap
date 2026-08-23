# catalog

目录服务：表与列的元数据管理。

## 职责

- 维护数据库级元数据：表名 → 表 schema（列名、类型、顺序）
- 提供元数据的增删查：`create_table`、`drop_table`、`get_table`、`list_tables`
- 持久化元数据（落盘为元数据文件），重启后可恢复
- 为 planner 提供列解析（name → column index）与类型校验

## 设计要点

- catalog 是 planner 与 storage 之间的桥梁：planner 查 catalog 解析列，storage 按 catalog 的 schema 组织数据
- 元数据变更需与存储文件保持一致（事务性可选）
- 线程安全：多查询并发读取元数据