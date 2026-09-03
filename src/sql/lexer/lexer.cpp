#include "lexer.h"

#include <cctype>

namespace simple_olap
{

    Lexer::Lexer(std::string_view sql) : sql_(sql) {}

    Token Lexer::NextToken()
    {
        SkipWhitespace();

        if (curridx_ >= sql_.size())
        {
            return Token{TokenType::END, "", curridx_};
        }

        char c = sql_[curridx_];

        // 标识符或关键字：字母或下划线开头
        if (std::isalpha(static_cast<unsigned char>(c)) || c == '_')
        {
            return ReadIdentifierOrKeyword();
        }

        // 数字字面量：整数或浮点
        if (std::isdigit(static_cast<unsigned char>(c)))
        {
            return ReadNumber();
        }

        // 字符串字面量：单引号包裹
        if (c == '\'')
        {
            return ReadString();
        }

        // 运算符与标点
        Token token{TokenType::END, "", curridx_};
        switch (c)
        {
        case '(':
            token = Token{TokenType::LPAREN, "(", curridx_};
            break;
        case ')':
            token = Token{TokenType::RPAREN, ")", curridx_};
            break;
        case ',':
            token = Token{TokenType::COMMA, ",", curridx_};
            break;
        case ';':
            token = Token{TokenType::SEMICOLON, ";", curridx_};
            break;
        case '*':
            token = Token{TokenType::STAR, "*", curridx_};
            break;
        case '+':
            token = Token{TokenType::PLUS, "+", curridx_};
            break;
        case '-':
            // 可能是注释 "--"
            if (curridx_ + 1 < sql_.size() && sql_[curridx_ + 1] == '-')
            {
                SkipWhitespace();
                return NextToken();
            }
            token = Token{TokenType::MINUS, "-", curridx_};
            break;
        case '=':
            token = Token{TokenType::EQ, "=", curridx_};
            break;
        case '<':
            if (curridx_ + 1 < sql_.size() && sql_[curridx_ + 1] == '=')
            {
                token = Token{TokenType::LE, "<=", curridx_};
                curridx_++;
            }
            else
            {
                token = Token{TokenType::LT, "<", curridx_};
            }
            break;
        case '>':
            if (curridx_ + 1 < sql_.size() && sql_[curridx_ + 1] == '=')
            {
                token = Token{TokenType::GE, ">=", curridx_};
                curridx_++;
            }
            else
            {
                token = Token{TokenType::GT, ">", curridx_};
            }
            break;
        case '!':
            if (curridx_ + 1 < sql_.size() && sql_[curridx_ + 1] == '=')
            {
                token = Token{TokenType::NE, "!=", curridx_};
                curridx_++;
            }
            break;
        default:
            // 未知字符，跳过
            curridx_++;
            return NextToken();
        }

        curridx_++;
        return token;
    }

    std::vector<Token> Lexer::Tokenize()
    {
        std::vector<Token> tokens;
        while (true)
        {
            Token token = NextToken();
            tokens.push_back(token);
            if (token.type == TokenType::END)
            {
                break;
            }
        }
        return tokens;
    }

    char Lexer::Peek() const
    {
        if (curridx_ >= sql_.size())
        {
            return '\0';
        }
        return sql_[curridx_];
    }

    char Lexer::Advance()
    {
        if (curridx_ >= sql_.size())
        {
            return '\0';
        }
        return sql_[curridx_++];
    }

    void Lexer::SkipWhitespace()
    {
        while (curridx_ < sql_.size() && std::isspace(static_cast<unsigned char>(sql_[curridx_])))
        {
            curridx_++;
        }
    }

    Token Lexer::ReadIdentifierOrKeyword()
    {
        uint32_t start = curridx_;
        while (curridx_ < sql_.size() &&
               (std::isalnum(static_cast<unsigned char>(sql_[curridx_])) || sql_[curridx_] == '_'))
        {
            curridx_++;
        }
        std::string text(sql_.substr(start, curridx_ - start));

        // 关键字识别
        if (text == "SELECT")
            return Token{TokenType::SELECT, text, start};
        if (text == "FROM")
            return Token{TokenType::FROM, text, start};
        if (text == "WHERE")
            return Token{TokenType::WHERE, text, start};
        if (text == "GROUP")
            return Token{TokenType::GROUP, text, start};
        if (text == "BY")
            return Token{TokenType::BY, text, start};
        if (text == "CREATE")
            return Token{TokenType::CREATE, text, start};
        if (text == "TABLE")
            return Token{TokenType::TABLE, text, start};
        if (text == "INSERT")
            return Token{TokenType::INSERT, text, start};
        if (text == "INTO")
            return Token{TokenType::INTO, text, start};
        if (text == "VALUES")
            return Token{TokenType::VALUES, text, start};

        return Token{TokenType::IDENTIFIER, text, start};
    }

    Token Lexer::ReadNumber()
    {
        uint32_t start = curridx_;
        bool is_float = false;

        while (curridx_ < sql_.size())
        {
            char c = sql_[curridx_];
            if (std::isdigit(static_cast<unsigned char>(c)))
            {
                curridx_++;
            }
            else if (c == '.' && !is_float)
            {
                is_float = true;
                curridx_++;
            }
            else
            {
                break;
            }
        }

        std::string text(sql_.substr(start, curridx_ - start));
        return Token{TokenType::FLOAT, text, start};
    }

    Token Lexer::ReadString()
    {
        // 跳过开头的单引号
        curridx_++;
        uint32_t start = curridx_;

        while (curridx_ < sql_.size() && sql_[curridx_] != '\'')
        {
            curridx_++;
        }

        std::string text(sql_.substr(start, curridx_ - start));

        // 跳过结尾的单引号
        if (curridx_ < sql_.size())
        {
            curridx_++;
        }

        return Token{TokenType::STRING, text, start};
    }

} // namespace simple_olap
