#include "token.h"
#include <vector>

namespace simple_olap
{
    class Lexer {
    public:
        explicit Lexer(std::string_view sql);
        /// @brief 获取下一个token。
        /// 大致流程：1.取 curridx_, 不停读取直到遇到“空格”或者“回车”。2.与 TokenType 对比，如果是关键字，数字，字符串。3.增加 curridx_ 。
        /// @return 处于 curridx_ 的token。
        Token NextToken();
        // 通过多次运行 NextToken() 得到结果
        std::vector<Token> Tokenize();

    private:
        char Peek() const;
        char Advance();
        void SkipWhitespace();
        Token ReadIdentifierOrKeyword();
        Token ReadNumber();
        Token ReadString();

        std::string_view sql_;
        uint32_t curridx_ = 0;   // 记录当前解析的位置
    };

} // namespace simple_olap
