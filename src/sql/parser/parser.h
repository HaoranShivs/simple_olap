#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "../../storage/datastructs.h"
#include "../ast/expression.h"
#include "../ast/statement.h"
#include "../lexer/lexer.h"
#include "../token.h"

namespace simple_olap {

// SQL 语法解析器：以递归下降方式把 Token 流构造成 AST。
class Parser {
  public:
    explicit Parser();

    // 解析总入口：按当前 Token 类型分发到具体语句解析函数，返回 AST 根节点。
    StatementPtr ParseStatement(std::vector<Token> tokens);

  private:
    // ---------- 语句级解析 ----------

    // 解析完整的 SELECT 语句，组装成 SelectStatement。
    std::unique_ptr<SelectStatement> ParseSelect();

    // 解析 CREATE TABLE 语句。
    std::unique_ptr<CreateTableStatement> ParseCreateTable();

    // 解析 INSERT INTO ... VALUES 语句。
    std::unique_ptr<InsertStatement> ParseInsert();

    // 解析单个投影项，如 a、b AS c、COUNT(*)。
    SelectItem ParseSelectItem();

    // ---------- 表达式解析 ----------

    // 解析表达式，处理二元运算符的优先级与结合性。
    ExprPtr ParseExpression();

    // 解析原子表达式：字面量、列名、函数调用、括号子表达式。
    ExprPtr ParsePrimaryExpression();

    // ---------- 数据类型解析 ----------

    // 解析列数据类型（INT/BIGINT/FLOAT/DOUBLE/VARCHAR），用于 CREATE TABLE。
    DataType ParseDataType();

    // ---------- Token 流操作 ----------

    // 向前查看第 offset 个 Token，不消耗。
    const Token& Peek(size_t offset = 0) const;

    // 消耗并返回当前 Token，指针后移一位。
    Token Consume();

    // 匹配成功则消耗并返回 true，否则不消耗返回 false（用于可选语法）。
    bool Match(TokenType type);

    // 强制匹配：不匹配则抛出 message（用于必需语法）。
    Token Expect(TokenType type, const std::string& message);

    // ---------- 成员变量 ----------

    // tokens_ 为词法分析结果，current_index_ 为当前读取位置。
    std::vector<Token> tokens_;
    uint32_t current_index_ = 0;
    Lexer lexer_;
};

} // namespace simple_olap
