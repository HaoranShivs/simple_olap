#include "statement.h"

namespace simple_olap
{

    std::string SelectStatement::ToString() const
    {
        std::string result = "SELECT ";
        for (size_t i = 0; i < select_list.size(); ++i)
        {
            if (i > 0)
                result += ", ";
            // 有别名用别名，否则用类型占位
            if (!select_list[i].alias.empty())
            {
                result += select_list[i].alias;
            }
            else
            {
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

    void SelectStatement::Accept(StatementVisitor *visitor) const
    {
        // 访问者模式预留，后续实现
        (void)visitor;
    }

} // namespace simple_olap
