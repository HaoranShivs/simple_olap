#pragma once

#include <cstdint>
#include <string>

namespace simple_olap {
enum class TokenType {
    END, // 输入结束

    // ---------- 标识符与字面量 ----------
    IDENTIFIER, // 表名、列名
    INTEGER,
    FLOAT,
    STRING, // 字符串字面量

    // ---------- 关键字 ----------
    SELECT,
    FROM,
    WHERE,
    GROUP,
    BY,
    CREATE,
    TABLE,
    INSERT,
    INTO,
    VALUES,
    AS, // 列别名: SELECT a AS x
    // 预留关键字（暂未实现）：IF / NOT / EXISTS，用于 CREATE TABLE

    // ---------- 聚合函数名 ----------
    SUM,
    COUNT,
    AVG,
    MIN,
    MAX,

    // ---------- 标点 ----------
    STAR,      // *
    COMMA,     // 逗号
    SEMICOLON, // 语句结束符：一次输入多条语句时用于分隔
    LPAREN,
    RPAREN, // 左右括号

    // ---------- 运算符 ----------
    EQ,
    NE,
    LT,
    LE,
    GT,
    GE,
    PLUS,
    MINUS,
    MUL,
    DIV
};

// 已废弃：源码位置信息暂未实现。
// struct SourceLocation {
//     uint32_t line = 1;
//     uint32_t column = 1;
// };

// 词法单元：类型 + 原文 + 在 SQL 文本中的起始下标。
struct Token {
    TokenType type;
    std::string text;
    uint32_t curridx_;
};
} // namespace simple_olap
