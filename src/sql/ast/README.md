# sql/ast

抽象语法树（AST）节点定义。

## 职责

- 定义 SQL 语句与表达式的树形结构：
  - 语句节点：`SelectStmt`、`CreateTableStmt`、`DropTableStmt` 等
  - 表达式节点：`ColumnRef`、`Literal`、`BinaryExpr`、`FunctionCall`、`AggregateExpr` 等
  - 子句节点：`WhereClause`、`GroupByClause`、`OrderByClause`、`LimitClause` 等
- 提供 AST 的遍历（visitor）基础设施

## 设计要点

- 节点携带源位置信息，用于错误诊断
- 保持 AST 与具体语言特性解耦，便于 planner 消费
- 不可变结构优先，避免意外共享修改