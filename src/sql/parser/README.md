# sql/parser

语法分析器：将 token 流解析为 AST。

## 职责

- 基于 token 流，按 SQL 文法（递归下降或 LALR）构建抽象语法树
- 支持 SELECT（含 WHERE / GROUP BY / HAVING / ORDER BY / LIMIT）、CREATE TABLE、DROP TABLE 等语句
- 语法错误报告（期望 token vs 实际 token）

## 设计要点

- 与 lexer 解耦：输入为 token 流，输出为 AST
- 表达式解析需正确处理运算符优先级与结合性
- 为 planner 提供结构化的 AST 节点