#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "../../sql/ast/boundstat.h"
#include "../plan_expression.h"

namespace simple_olap {

struct SimplePredicate {
    uint32_t column_index = 0;
    CmpOp op = CmpOp::EQ;
    PlanLiteralValue value;
};

class LogicalPlan {
  public:
    enum class Type : uint8_t {
        SCAN,
        FILTER,
        PROJECT,
        AGGREGATE,
        INSERT,
        CREATE_TABLE,
    };

    explicit LogicalPlan(Type type) : type_(type) {}
    virtual ~LogicalPlan() = default;

    Type GetType() const {
        return type_;
    }
    virtual std::string ToString(size_t indent = 0) const = 0;

  private:
    Type type_;
};

using LogicalPlanPtr = std::unique_ptr<LogicalPlan>;

// class LogicalScan final : public LogicalPlan {
//   public:
//     explicit LogicalScan(uint32_t table_oid) : LogicalPlan(Type::SCAN), table_oid_(table_oid) {}

//     uint32_t GetTableOid() const {
//         return table_oid_;
//     }
//     const std::vector<uint32_t>& GetRequiredColumns() const {
//         return required_columns_;
//     }
//     const std::optional<SimplePredicate>& GetPushedPredicate() const {
//         return pushed_predicate_;
//     }

//     void SetRequiredColumns(std::vector<uint32_t> columns) {
//         required_columns_ = std::move(columns);
//     }
//     void SetPushedPredicate(SimplePredicate predicate) {
//         pushed_predicate_ = std::move(predicate);
//     }

//     std::string ToString(size_t indent = 0) const override;

//   private:
//     uint32_t table_oid_;
//     std::vector<uint32_t> required_columns_;
//     std::optional<SimplePredicate> pushed_predicate_;
// };

class LogicalScan final : public LogicalPlan {
  public:
    explicit LogicalScan(uint32_t table_oid) : LogicalPlan(Type::SCAN), table_oid_(table_oid) {}

    uint32_t GetTableOid() const {
        return table_oid_;
    }

    const std::vector<uint32_t>& GetRequiredColumns() const {
        return required_columns_;
    }

    const std::vector<SimplePredicate>& GetPushedPredicates() const {
        return pushed_predicates_;
    }

    void SetRequiredColumns(std::vector<uint32_t> columns) {
        required_columns_ = std::move(columns);
    }

    void AddPushedPredicate(SimplePredicate predicate) {
        pushed_predicates_.push_back(std::move(predicate));
    }

    std::string ToString(size_t indent = 0) const override;

  private:
    uint32_t table_oid_;

    std::vector<uint32_t> required_columns_;

    std::vector<SimplePredicate> pushed_predicates_;
};

class LogicalFilter final : public LogicalPlan {
  public:
    LogicalFilter(PlanExprPtr predicate, LogicalPlanPtr child)
        : LogicalPlan(Type::FILTER), predicate_(std::move(predicate)), child_(std::move(child)) {}

    const PlanExpr& GetPredicate() const {
        return *predicate_;
    }
    PlanExpr& MutablePredicate() {
        return *predicate_;
    }
    const LogicalPlan& GetChild() const {
        return *child_;
    }
    LogicalPlanPtr& MutableChild() {
        return child_;
    }
    void SetPredicate(PlanExprPtr predicate) {
        predicate_ = std::move(predicate);
    }

    std::string ToString(size_t indent = 0) const override;

  private:
    PlanExprPtr predicate_;
    LogicalPlanPtr child_;
};

class LogicalProject final : public LogicalPlan {
  public:
    LogicalProject(std::vector<NamedPlanExpr> outputs, LogicalPlanPtr child)
        : LogicalPlan(Type::PROJECT), outputs_(std::move(outputs)), child_(std::move(child)) {}

    const std::vector<NamedPlanExpr>& GetOutputs() const {
        return outputs_;
    }
    const LogicalPlan& GetChild() const {
        return *child_;
    }
    LogicalPlanPtr& MutableChild() {
        return child_;
    }

    std::string ToString(size_t indent = 0) const override;

  private:
    std::vector<NamedPlanExpr> outputs_;
    LogicalPlanPtr child_;
};

class LogicalAggregate final : public LogicalPlan {
  public:
    LogicalAggregate(std::vector<PlanExprPtr> group_by, std::vector<NamedPlanExpr> outputs, LogicalPlanPtr child)
        : LogicalPlan(Type::AGGREGATE), group_by_(std::move(group_by)), outputs_(std::move(outputs)),
          child_(std::move(child)) {}

    const std::vector<PlanExprPtr>& GetGroupBy() const {
        return group_by_;
    }
    const std::vector<NamedPlanExpr>& GetOutputs() const {
        return outputs_;
    }
    const LogicalPlan& GetChild() const {
        return *child_;
    }
    LogicalPlanPtr& MutableChild() {
        return child_;
    }

    std::string ToString(size_t indent = 0) const override;

  private:
    std::vector<PlanExprPtr> group_by_;
    std::vector<NamedPlanExpr> outputs_;
    LogicalPlanPtr child_;
};

class LogicalInsert final : public LogicalPlan {
  public:
    LogicalInsert(uint32_t table_oid, std::vector<uint32_t> target_columns, std::vector<std::vector<PlanExprPtr>> rows)
        : LogicalPlan(Type::INSERT), table_oid_(table_oid), target_columns_(std::move(target_columns)),
          rows_(std::move(rows)) {}

    uint32_t GetTableOid() const {
        return table_oid_;
    }
    const std::vector<uint32_t>& GetTargetColumns() const {
        return target_columns_;
    }
    const std::vector<std::vector<PlanExprPtr>>& GetRows() const {
        return rows_;
    }

    std::string ToString(size_t indent = 0) const override;

  private:
    uint32_t table_oid_;
    std::vector<uint32_t> target_columns_;
    std::vector<std::vector<PlanExprPtr>> rows_;
};

class LogicalCreateTable final : public LogicalPlan {
  public:
    LogicalCreateTable(std::string table_name, std::vector<BoundColumnDef> columns)
        : LogicalPlan(Type::CREATE_TABLE), table_name_(std::move(table_name)), columns_(std::move(columns)) {}

    const std::string& GetTableName() const {
        return table_name_;
    }
    const std::vector<BoundColumnDef>& GetColumns() const {
        return columns_;
    }

    std::string ToString(size_t indent = 0) const override;

  private:
    std::string table_name_;
    std::vector<BoundColumnDef> columns_;
};

} // namespace simple_olap
