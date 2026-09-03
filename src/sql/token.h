#pragma once

#include <cstdint>
#include <string>

namespace simple_olap
{
    enum class TokenType {
        END,
        IDENTIFIER, // 表名，列名
        INTEGER,
        FLOAT,
        STRING, // 字面量

        SELECT, FROM, WHERE, GROUP, BY,
        CREATE, TABLE, INSERT, INTO, VALUES,
        // IF, NOT, EXIST, //对应 creat table

        SUM, COUNT, AVG, MIN, MAX,

        STAR,   // *号
        COMMA,  // 逗号
        SEMICOLON,  // 语句结束，有时候一次输入多条语句，这时候需要这个，表示单个语句的结束
        LPAREN,
        RPAREN, //左右括号

        EQ, NE, LT, LE, GT, GE,
        PLUS, MINUS, MUL, DIV
    };

    // struct SourceLocation {
    //     uint32_t line = 1;
    //     uint32_t column = 1;
    // };

    struct Token {
        TokenType type;
        std::string text;
        uint32_t curridx_;
    };
} // namespace simple_olap
