#include "lexer.h"

#include <cctype>

namespace simple_olap {

// ==========================================
// 公共接口
// ==========================================
Lexer::Lexer() = default;

std::vector<Token> Lexer::Lex(std::string_view sql) {
    sql_ = sql;
    curridx_ = 0;
    return Tokenize();
}

// ==========================================
// Token 扫描
// ==========================================
Token Lexer::NextToken() {
    SkipWhitespace();

    if (curridx_ >= sql_.size()) {
        return Token{TokenType::END, "", curridx_};
    }

    char c = sql_[curridx_];

    // 标识符或关键字：字母或下划线开头
    if (std::isalpha(static_cast<unsigned char>(c)) || c == '_') {
        return ReadIdentifierOrKeyword();
    }

    // 数字字面量：整数或浮点
    if (std::isdigit(static_cast<unsigned char>(c))) {
        return ReadNumber();
    }

    // 字符串字面量：单引号包裹
    if (c == '\'') {
        return ReadString();
    }

    // 运算符与标点
    Token token{TokenType::END, "", curridx_};
    switch (c) {
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
        // 行注释 "--"：当前实现不跳过整行内容，仅继续取下一个 Token。
        if (curridx_ + 1 < sql_.size() && sql_[curridx_ + 1] == '-') {
            SkipWhitespace();
            return NextToken();
        }
        token = Token{TokenType::MINUS, "-", curridx_};
        break;
    case '=':
        token = Token{TokenType::EQ, "=", curridx_};
        break;
    case '<':
        if (curridx_ + 1 < sql_.size() && sql_[curridx_ + 1] == '=') {
            token = Token{TokenType::LE, "<=", curridx_};
            curridx_++;
        } else {
            token = Token{TokenType::LT, "<", curridx_};
        }
        break;
    case '>':
        if (curridx_ + 1 < sql_.size() && sql_[curridx_ + 1] == '=') {
            token = Token{TokenType::GE, ">=", curridx_};
            curridx_++;
        } else {
            token = Token{TokenType::GT, ">", curridx_};
        }
        break;
    case '!':
        if (curridx_ + 1 < sql_.size() && sql_[curridx_ + 1] == '=') {
            token = Token{TokenType::NE, "!=", curridx_};
            curridx_++;
        }
        break;
    default:
        // 未知字符：跳过并继续。
        curridx_++;
        return NextToken();
    }

    curridx_++;
    return token;
}

std::vector<Token> Lexer::Tokenize() {
    std::vector<Token> tokens;
    while (true) {
        Token token = NextToken();
        tokens.push_back(token);
        if (token.type == TokenType::END) {
            break;
        }
    }
    return tokens;
}

// ==========================================
// 字符读取辅助
// ==========================================
char Lexer::Peek() const {
    if (curridx_ >= sql_.size()) {
        return '\0';
    }
    return sql_[curridx_];
}

char Lexer::Advance() {
    if (curridx_ >= sql_.size()) {
        return '\0';
    }
    return sql_[curridx_++];
}

void Lexer::SkipWhitespace() {
    while (curridx_ < sql_.size() && std::isspace(static_cast<unsigned char>(sql_[curridx_]))) {
        curridx_++;
    }
}

// ==========================================
// 各类 Token 读取
// ==========================================
Token Lexer::ReadIdentifierOrKeyword() {
    uint32_t start = curridx_;
    while (curridx_ < sql_.size() &&
           (std::isalnum(static_cast<unsigned char>(sql_[curridx_])) || sql_[curridx_] == '_')) {
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
    if (text == "AS")
        return Token{TokenType::AS, text, start};

    return Token{TokenType::IDENTIFIER, text, start};
}

Token Lexer::ReadNumber() {
    uint32_t start = curridx_;
    bool is_float = false;

    while (curridx_ < sql_.size()) {
        char c = sql_[curridx_];
        if (std::isdigit(static_cast<unsigned char>(c))) {
            curridx_++;
        } else if (c == '.' && !is_float) {
            is_float = true;
            curridx_++;
        } else {
            break;
        }
    }

    std::string text(sql_.substr(start, curridx_ - start));
    // 区分整数与浮点：整数 -> INTEGER，浮点 -> FLOAT。
    // 若不区分，INSERT INTO t(a INT) VALUES (1) 会因字面量被当成 DOUBLE 而类型不匹配。
    if (!is_float) {
        return Token{TokenType::INTEGER, text, start};
    }
    return Token{TokenType::FLOAT, text, start};
}

Token Lexer::ReadString() {
    // 跳过开头的单引号
    curridx_++;
    uint32_t start = curridx_;

    while (curridx_ < sql_.size() && sql_[curridx_] != '\'') {
        curridx_++;
    }

    std::string text(sql_.substr(start, curridx_ - start));

    // 跳过结尾的单引号
    if (curridx_ < sql_.size()) {
        curridx_++;
    }

    return Token{TokenType::STRING, text, start};
}

} // namespace simple_olap
