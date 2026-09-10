#pragma once

#include "../token.h"
#include <vector>

namespace simple_olap {
// SQL 词法分析器：把 SQL 文本切分成 Token 序列。
class Lexer {
  public:
    explicit Lexer();

    // 对外入口：对 sql 做词法分析并返回全部 Token（以 END 结尾）。
    std::vector<Token> Lex(std::string_view sql);

  private:
    // ---------- Token 扫描 ----------
    // 取出下一个 Token：跳过空白，再按首字符分派到标识符/数字/字符串/运算符分支。
    Token NextToken();

    // 循环调用 NextToken() 直到 END。
    std::vector<Token> Tokenize();

    // ---------- 字符读取辅助 ----------
    // 查看当前字符（不前进），越界返回 '\0'。
    char Peek() const;

    // 返回当前字符并前进一位，越界返回 '\0'。
    char Advance();

    // 跳过空白字符。
    void SkipWhitespace();

    // ---------- 各类 Token 读取 ----------
    Token ReadIdentifierOrKeyword();
    Token ReadNumber();
    Token ReadString();

    // ---------- 成员变量 ----------
    std::string_view sql_; // 待分析的 SQL 文本
    uint32_t curridx_ = 0; // 记录当前解析的位置
};

} // namespace simple_olap
