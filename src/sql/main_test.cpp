// ============================================================
// src/sql 模块局部测试：Lexer -> Parser -> AST
// 编译: g++ -std=c++17 -I src/sql src/sql/lexer/lexer.cpp \
//           src/sql/parser/parser.cpp src/sql/main_test.cpp -o main_test
// ============================================================

#include <iostream>

#include "lexer/lexer.h"
#include "parser/parser.h"

using namespace simple_olap;

static int passed = 0;
static int failed = 0;

#define CHECK(cond, msg)                                  \
    do                                                    \
    {                                                     \
        if (cond)                                         \
        {                                                 \
            passed++;                                     \
            std::cout << "[PASS] " << msg << std::endl;   \
        }                                                 \
        else                                              \
        {                                                 \
            failed++;                                     \
            std::cout << "[FAIL] " << msg << std::endl;   \
        }                                                 \
    } while (0)

// ---------- Lexer 测试 ----------

static void TestLexerBasic()
{
    Lexer lexer("SELECT a, b FROM t;");
    auto tokens = lexer.Tokenize();

    CHECK(tokens.size() == 8, "Tokenize: token count == 8 (7 tokens + END)");
    CHECK(tokens[0].type == TokenType::SELECT, "Token[0] is SELECT");
    CHECK(tokens[1].type == TokenType::IDENTIFIER && tokens[1].text == "a", "Token[1] is identifier 'a'");
    CHECK(tokens[2].type == TokenType::COMMA, "Token[2] is COMMA");
    CHECK(tokens[4].type == TokenType::FROM, "Token[4] is FROM");
    CHECK(tokens[5].type == TokenType::IDENTIFIER && tokens[5].text == "t", "Token[5] is identifier 't'");
    CHECK(tokens[6].type == TokenType::SEMICOLON, "Token[6] is SEMICOLON");
    CHECK(tokens[8].type == TokenType::END, "Last token is END");
}

static void TestLexerNumbersAndStrings()
{
    Lexer lexer("WHERE age > 18 AND name = 'tom'");
    auto tokens = lexer.Tokenize();

    CHECK(tokens[3].type == TokenType::FLOAT && tokens[3].text == "18", "Number literal '18'");
    CHECK(tokens[7].type == TokenType::STRING && tokens[7].text == "tom", "String literal 'tom'");
}

static void TestLexerOperators()
{
    Lexer lexer("a <= 1 AND b >= 2 AND c != 3");
    auto tokens = lexer.Tokenize();

    CHECK(tokens[1].type == TokenType::LE, "Operator '<='");
    CHECK(tokens[5].type == TokenType::GE, "Operator '>='");
    CHECK(tokens[9].type == TokenType::NE, "Operator '!='");
}

// ---------- Parser 测试 ----------

static void TestParseSimpleSelect()
{
    Lexer lexer("SELECT a, b FROM t;");
    Parser parser(lexer.Tokenize());
    auto stmt = parser.ParseStatement();

    CHECK(stmt != nullptr, "Parse: statement is not null");
    CHECK(stmt->GetType() == Statement::Type::SELECT, "Parse: statement type is SELECT");

    auto *select = static_cast<SelectStatement *>(stmt.get());
    CHECK(select->table_name == "t", "Parse: table name is 't'");
    CHECK(select->select_list.size() == 2, "Parse: select list has 2 items");
}

static void TestParseWhereAndGroupBy()
{
    Lexer lexer("SELECT city, SUM(amount) FROM orders WHERE price > 10 GROUP BY city");
    Parser parser(lexer.Tokenize());
    auto stmt = parser.ParseStatement();

    auto *select = static_cast<SelectStatement *>(stmt.get());
    CHECK(select->where_clause != nullptr, "Parse: WHERE clause exists");
    CHECK(select->group_by.size() == 1, "Parse: GROUP BY has 1 expression");
    CHECK(select->select_list.size() == 2, "Parse: select list has 2 items");

    // 第二个投影项应为聚合函数 SUM
    auto *agg = static_cast<AggFuncExpr *>(select->select_list[1].expr.get());
    CHECK(agg->type == Expr::Type::AGG_FUNC && agg->agg_type == AggType::SUM, "Parse: SUM(amount) is AggFuncExpr");
}

static void TestParseStar()
{
    Lexer lexer("SELECT * FROM t;");
    Parser parser(lexer.Tokenize());
    auto stmt = parser.ParseStatement();

    auto *select = static_cast<SelectStatement *>(stmt.get());
    CHECK(select->select_list.size() == 1, "Parse: SELECT * has 1 item");
    auto *col = static_cast<ColumnRefExpr *>(select->select_list[0].expr.get());
    CHECK(col->column_name == "*", "Parse: star is ColumnRefExpr('*')");
}

static void TestParseError()
{
    bool threw = false;
    try
    {
        Lexer lexer("SELECT a FROM;"); // FROM 后缺少表名
        Parser parser(lexer.Tokenize());
        parser.ParseStatement();
    }
    catch (const std::runtime_error &)
    {
        threw = true;
    }
    CHECK(threw, "Parse: missing table name throws error");
}

int main()
{
    std::cout << "===== Lexer Tests =====" << std::endl;
    TestLexerBasic();
    TestLexerNumbersAndStrings();
    TestLexerOperators();

    std::cout << "\n===== Parser Tests =====" << std::endl;
    TestParseSimpleSelect();
    TestParseWhereAndGroupBy();
    TestParseStar();
    TestParseError();

    std::cout << "\n===== Result: " << passed << " passed, " << failed << " failed =====" << std::endl;
    return failed == 0 ? 0 : 1;
}
