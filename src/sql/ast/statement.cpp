#include "statement.h"

namespace simple_olap {

// 输出 SELECT 的简化文本：表达式与条件均以占位符表示，仅用于调试。
std::string SelectStatement::ToString() const {
    std::string result = "SELECT ";
    for (size_t i = 0; i < select_list.size(); ++i) {
        if (i > 0)
            result += ", ";
        // 有别名用别名，否则用占位符 <expr>。
        if (!select_list[i].alias.empty()) {
            result += select_list[i].alias;
        } else {
            result += "<expr>";
        }
    }
    result += " FROM " + table_name;
    if (where_clause)
        result += " WHERE <cond>";
    if (!group_by.empty())
        result += " GROUP BY <keys>";
    return result;
}

// Visitor 暂未实现，空定义仅为满足链接。
void SelectStatement::Accept(StatementVisitor* visitor) const {
    (void)visitor;
}

} // namespace simple_olap
