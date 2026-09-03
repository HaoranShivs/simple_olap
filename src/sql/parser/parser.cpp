#include "parser.h"

#include <optional>
#include <stdexcept>

namespace simple_olap
{

    // ============================================================
    // 构造与入口
    // ============================================================

    Parser::Parser(std::vector<Token> tokens)
        : tokens_(std::move(tokens)), current_index_(0) {}

    StatementPtr Parser::ParseStatement()
    {
        if (Match(TokenType::SELECT))
        {
            return ParseSelect();
        }
        if (Match(TokenType::CREATE))
        {
            return ParseCreateTable();
        }
        if (Match(TokenType::INSERT))
        {
            return ParseInsert();
        }
        // 后续支持: UPDATE, DELETE 等
        throw std::runtime_error("Unsupported statement type at token: " + Peek().text);
    }

    // ============================================================
    // SELECT 语句解析
    // ============================================================

    std::unique_ptr<SelectStatement> Parser::ParseSelect()
    {
        auto stmt = std::make_unique<SelectStatement>();

        // 1. SELECT 子句：投影列表（此处 SELECT 已被消耗）
        stmt->select_list.push_back(ParseSelectItem());
        while (Match(TokenType::COMMA))
        {
            stmt->select_list.push_back(ParseSelectItem());
        }

        // 2. FROM 子句：表名（必需）
        Expect(TokenType::FROM, "Expected FROM clause");
        stmt->table_name = Expect(TokenType::IDENTIFIER, "Expected table name after FROM").text;

        // 3. WHERE 子句（可选）
        if (Match(TokenType::WHERE))
        {
            stmt->where_clause = ParseExpression();
        }

        // 4. GROUP BY 子句（可选）
        if (Match(TokenType::GROUP))
        {
            Expect(TokenType::BY, "Expected BY after GROUP");
            stmt->group_by.push_back(ParseExpression());
            while (Match(TokenType::COMMA))
            {
                stmt->group_by.push_back(ParseExpression());
            }
        }

        // 5. 语句结束（可选分号）
        Match(TokenType::SEMICOLON);

        return stmt;
    }

    std::unique_ptr<CreateTableStatement> Parser::ParseCreateTable()
    {
        auto stmt = std::make_unique<CreateTableStatement>();

        Expect(TokenType::TABLE, "Expected 'TABLE' after CREATE");

        // 检查 IF NOT EXISTS（暂不支持，词法层未提供 IF/NOT/EXISTS 关键字）
        // if (Match(TokenType::IF)) { ... }

        stmt->table_name = Expect(TokenType::IDENTIFIER, "Expected table name").text;
        Expect(TokenType::LPAREN, "Expected '(' after table name");

        // 循环解析列定义
        do
        {
            ColumnSchema col;
            col.column_id = static_cast<uint32_t>(stmt->columns.size());
            col.name = Expect(TokenType::IDENTIFIER, "Expected column name").text;

            // 解析数据类型
            col.type = ParseDataType();

            // 约束（PRIMARY KEY / NOT NULL）暂不支持：词法层未提供对应关键字
            stmt->columns.push_back(std::move(col));
        } while (Match(TokenType::COMMA)); // 如果有逗号，继续解析下一列

        Expect(TokenType::RPAREN, "Expected ')' after columns");

        // 可选：解析 ORDER BY (sort_keys) ...

        return stmt;
    }

    // 解析列数据类型：INT / BIGINT / FLOAT / DOUBLE / VARCHAR
    // 类型名在词法层是普通 IDENTIFIER，这里按文本匹配
    DataType Parser::ParseDataType()
    {
        const Token &token = Expect(TokenType::IDENTIFIER, "Expected data type");

        // 转大写以便大小写不敏感匹配
        std::string upper = token.text;
        for (char &c : upper)
        {
            c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        }

        if (upper == "INT" || upper == "INTEGER")
            return DataType::INT32;
        if (upper == "BIGINT")
            return DataType::INT64;
        if (upper == "FLOAT")
            return DataType::FLOAT;
        if (upper == "DOUBLE")
            return DataType::DOUBLE;
        if (upper == "VARCHAR")
            return DataType::VARCHAR;

        throw std::runtime_error("Unknown data type: " + token.text);
    }

    std::unique_ptr<InsertStatement> Parser::ParseInsert()
    {
        auto stmt = std::make_unique<InsertStatement>();

        // 1. 期望 INTO（ParseStatement 已消耗 INSERT Token）
        Expect(TokenType::INTO, "Expected 'INTO' after INSERT");

        // 2. 解析表名
        stmt->table_name = Expect(TokenType::IDENTIFIER, "Expected table name").text;

        // 3. 可选：解析列名列表
        // 语法：INSERT INTO t (a, b) VALUES ...
        if (Match(TokenType::LPAREN))
        {
            do
            {
                stmt->columns.push_back(Expect(TokenType::IDENTIFIER, "Expected column name").text);
            } while (Match(TokenType::COMMA));

            Expect(TokenType::RPAREN, "Expected ')' after column list");
        }

        // 4. 期望 VALUES 关键字
        Expect(TokenType::VALUES, "Expected 'VALUES' keyword");

        // 5. 解析多行值列表
        // 语法：VALUES (1, 'a'), (2, 'b')
        do
        {
            Expect(TokenType::LPAREN, "Expected '(' before row values");

            std::vector<ExprPtr> row;

            // 解析单行内的多个值
            do
            {
                row.push_back(ParseExpression());
            } while (Match(TokenType::COMMA));

            Expect(TokenType::RPAREN, "Expected ')' after row values");

            stmt->values.push_back(std::move(row));

        } while (Match(TokenType::COMMA)); // 如果还有逗号，继续解析下一行

        // 语句结束（可选分号）
        Match(TokenType::SEMICOLON);

        return stmt;
    }

    SelectItem Parser::ParseSelectItem()
    {
        // SELECT * ：全列投影
        if (Peek().type == TokenType::STAR)
        {
            Consume();
            auto star = std::make_unique<ColumnRefExpr>("*");
            return SelectItem(std::move(star));
        }

        // 普通投影项：表达式 [AS 别名]
        ExprPtr expr = ParseExpression();

        std::string alias;
        if (Peek().type == TokenType::IDENTIFIER)
        {
            // 隐式别名: SELECT a b
            alias = Consume().text;
        }

        return SelectItem(std::move(expr), std::move(alias));
    }

    // ============================================================
    // 表达式解析
    // ============================================================

    ExprPtr Parser::ParseExpression()
    {
        ExprPtr left = ParsePrimaryExpression();

        // 二元运算符（简化版：不区分优先级，后续可拆分为多层）
        while (true)
        {
            TokenType type = Peek().type;
            std::optional<BinaryOpExpr::OpType> op;
            switch (type)
            {
            case TokenType::PLUS:
                op = BinaryOpExpr::OpType::ADD;
                break;
            case TokenType::MINUS:
                op = BinaryOpExpr::OpType::SUB;
                break;
            case TokenType::EQ:
                op = BinaryOpExpr::OpType::EQ;
                break;
            case TokenType::GT:
                op = BinaryOpExpr::OpType::GT;
                break;
            case TokenType::LT:
                op = BinaryOpExpr::OpType::LT;
                break;
            default:
                break;
            }

            if (!op.has_value())
            {
                break;
            }

            Consume();
            ExprPtr right = ParsePrimaryExpression();
            left = std::make_unique<BinaryOpExpr>(*op, std::move(left), std::move(right));
        }

        return left;
    }

    ExprPtr Parser::ParsePrimaryExpression()
    {
        const Token &token = Peek();

        switch (token.type)
        {
        case TokenType::INTEGER:
        {
            int32_t value = std::stoi(Consume().text);
            return std::make_unique<LiteralExpr>(value);
        }
        case TokenType::FLOAT:
        {
            double value = std::stod(Consume().text);
            return std::make_unique<LiteralExpr>(value);
        }
        case TokenType::STRING:
        {
            std::string value = Consume().text;
            return std::make_unique<LiteralExpr>(std::move(value));
        }
        case TokenType::IDENTIFIER:
        {
            std::string name = Consume().text;
            // 函数调用: COUNT(a) / SUM(a) 等
            if (Peek().type == TokenType::LPAREN)
            {
                Consume(); // '('
                ExprPtr arg = ParseExpression();
                Expect(TokenType::RPAREN, "Expected ')' after function argument");

                AggType agg_type = AggType::INVALID;
                if (name == "SUM")
                    agg_type = AggType::SUM;
                else if (name == "COUNT")
                    agg_type = AggType::COUNT;
                else if (name == "AVG")
                    agg_type = AggType::AVG;
                else if (name == "MIN")
                    agg_type = AggType::MIN;
                else if (name == "MAX")
                    agg_type = AggType::MAX;

                if (agg_type == AggType::INVALID)
                {
                    throw std::runtime_error("Unknown function: " + name);
                }
                return std::make_unique<AggFuncExpr>(agg_type, std::move(arg));
            }
            return std::make_unique<ColumnRefExpr>(std::move(name));
        }
        case TokenType::LPAREN:
        {
            Consume(); // '('
            ExprPtr expr = ParseExpression();
            Expect(TokenType::RPAREN, "Expected ')'");
            return expr;
        }
        default:
            throw std::runtime_error("Unexpected token in expression: " + token.text);
        }
    }

    // ============================================================
    // Token 流操作
    // ============================================================

    const Token &Parser::Peek(size_t offset) const
    {
        size_t index = current_index_ + offset;
        if (index >= tokens_.size())
        {
            // 越界时返回最后一个 END token
            static const Token end_token{TokenType::END, "", 0};
            return end_token;
        }
        return tokens_[index];
    }

    Token Parser::Consume()
    {
        Token token = Peek();
        if (current_index_ < tokens_.size())
        {
            current_index_++;
        }
        return token;
    }

    bool Parser::Match(TokenType type)
    {
        if (Peek().type == type)
        {
            Consume();
            return true;
        }
        return false;
    }

    Token Parser::Expect(TokenType type, const std::string &message)
    {
        if (Peek().type != type)
        {
            throw std::runtime_error(message + ", got: " + Peek().text);
        }
        return Consume();
    }

} // namespace simple_olap
