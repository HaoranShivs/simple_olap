#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "../token.h"
#include "../ast/expression.h"
#include "../ast/statement.h"

namespace simple_olap
{

    class Parser
    {
    public:
        explicit Parser(std::vector<Token> tokens);

        /// @brief 解析总入口。根据当前 Token 的类型（如 SELECT, INSERT）进行分发（Dispatch），
        /// 调用具体的解析函数，并返回代表抽象语法树（AST）根节点的智能指针。
        StatementPtr ParseStatement();

    private:
        // ---------- 语句级解析 ----------

        /// @brief 解析完整的 SELECT 语句。负责按顺序解析 SELECT (投影列), FROM (表), WHERE (过滤),
        /// GROUP BY (分组), HAVING (聚合过滤), ORDER BY (排序), LIMIT (限制) 等子句，并组装成 SelectStatement 节点。
        std::unique_ptr<SelectStatement> ParseSelect();

        std::unique_ptr<CreateTableStatement> ParseCreateTable();

        std::unique_ptr<InsertStatement> ParseInsert();

        /// @brief 解析 SELECT 子句中的单个投影项。例如在 SELECT a, b AS c, count(*) 中，
        /// 它负责解析出 a、b AS c（包含别名）或 count(*)。
        SelectItem ParseSelectItem();

        // ---------- 表达式级解析 ----------

        /// @brief 解析复合表达式。处理二元运算符（如 +, -, AND, OR, =, < 等），负责处理运算符优先级和结合性，构建表达式树。
        ExprPtr ParseExpression();

        /// @brief 解析基本（原子）表达式。作为 ParseExpression 的底层递归基，处理数字/字符串字面量、
        /// 列名标识符、函数调用（如 COUNT(a)）、以及括号包裹的子表达式 (a + b)。
        ExprPtr ParsePrimaryExpression();

        // ---------- Token 流操作 ----------

        /// @brief 向前查看（Lookahead）。不消耗 Token，返回当前指针位置向后偏移 offset 的 Token。
        /// 常用于预测（Predict）下一步的语法结构（例如看到 ( 预测是函数调用还是分组表达式）。
        /// @param offset 偏移量
        const Token &Peek(size_t offset = 0) const;

        /// @brief 消耗当前 Token。返回当前 Token，并将索引 current_index_ 向后移动一位。
        Token Consume();

        /// @brief 尝试匹配并消耗。如果当前 Token 类型与 type 一致，则消耗它并返回 true,
        /// 否则不消耗并返回 false。常用于可选语法（如 LIMIT 子句、AS 别名）。
        /// @param type 匹配类型
        bool Match(TokenType type);

        /// @brief 强制匹配并消耗。如果当前 Token 类型与 type 一致，则消耗并返回；如果不一致，则触发错误
        /// （抛出异常或记录错误信息 message）。常用于必需语法（如 FROM 后面必须跟表名）。
        /// @param type 匹配类型
        /// @param message 错误信息
        Token Expect(TokenType type, const std::string &message);

        // ---------- 成员变量 ----------

        std::vector<Token> tokens_;
        uint32_t current_index_ = 0;
    };

} // namespace simple_olap
