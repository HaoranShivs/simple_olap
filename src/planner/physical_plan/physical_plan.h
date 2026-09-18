#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "../logical_plan/logical_plan.h"

namespace simple_olap {

// 物理计划节点基类：描述「怎么执行」，与具体算子一一对应。
class PhysicalPlan {
  public:
    enum class Type : uint8_t {
        SEQ_SCAN,
        INDEX_SCAN,
        FILTER,
        PROJECT,
        HASH_AGGREGATE,
        INSERT,
        CREATE_TABLE,
    };

    explicit PhysicalPlan(Type type) : type_(type) {}
    virtual ~PhysicalPlan() = default;

    Type GetType() const {
        return type_;
    }
    virtual std::string ToString(size_t indent = 0) const = 0;

  private:
    Type type_;
};

using PhysicalPlanPtr = std::unique_ptr<PhysicalPlan>;

// 已废弃：单 predicate 的旧版 PhysicalSeqScan，被下方支持多 predicate 的版本取代。
// 确认无引用后可整体删除。
// class PhysicalSeqScan final : public PhysicalPlan {
//   public:
//     PhysicalSeqScan(uint32_t table_oid, std::vector<uint32_t> columns, std::optional<SimplePredicate> predicate)
//         : PhysicalPlan(Type::SEQ_SCAN), table_oid_(table_oid), columns_(std::move(columns)),
//           predicate_(std::move(predicate)) {}

//     uint32_t GetTableOid() const {
//         return table_oid_;
//     }
//     const std::vector<uint32_t>& GetColumns() const {
//         return columns_;
//     }
//     const std::optional<SimplePredicate>& GetPredicate() const {
//         return predicate_;
//     }

//     std::string ToString(size_t indent = 0) const override;

//   private:
//     uint32_t table_oid_;
//     std::vector<uint32_t> columns_;
//     std::optional<SimplePredicate> predicate_;
// };

class PhysicalSeqScan final : public PhysicalPlan {
  public:
    PhysicalSeqScan(uint32_t table_oid, std::vector<uint32_t> columns, std::vector<SimplePredicate> predicates)
        : PhysicalPlan(Type::SEQ_SCAN), table_oid_(table_oid), columns_(std::move(columns)),
          predicates_(std::move(predicates)) {}

    uint32_t GetTableOid() const {
        return table_oid_;
    }

    const std::vector<uint32_t>& GetColumns() const {
        return columns_;
    }

    const std::vector<SimplePredicate>& GetPredicates() const {
        return predicates_;
    }

    std::string ToString(size_t indent = 0) const override;

  private:
    uint32_t table_oid_;

    std::vector<uint32_t> columns_;

    std::vector<SimplePredicate> predicates_;
};

// 索引点查：由 Access Path Selection 在「pushed predicates 完整覆盖某个键」时
// 替代 SeqScan 生成。lookup_values 与键列顺序一一对应；
// residual_predicates 是未被 lookup 消耗、仍需逐行精确判断的谓词。
class PhysicalIndexScan final : public PhysicalPlan {
  public:
    PhysicalIndexScan(uint32_t table_oid, KeyId key_id, std::vector<PlanLiteralValue> lookup_values,
                      std::vector<uint32_t> columns, std::vector<SimplePredicate> residual_predicates)
        : PhysicalPlan(Type::INDEX_SCAN), table_oid_(table_oid), key_id_(key_id),
          lookup_values_(std::move(lookup_values)), columns_(std::move(columns)),
          residual_predicates_(std::move(residual_predicates)) {}

    uint32_t GetTableOid() const {
        return table_oid_;
    }

    KeyId GetKeyId() const {
        return key_id_;
    }

    const std::vector<PlanLiteralValue>& GetLookupValues() const {
        return lookup_values_;
    }

    const std::vector<uint32_t>& GetColumns() const {
        return columns_;
    }

    const std::vector<SimplePredicate>& GetResidualPredicates() const {
        return residual_predicates_;
    }

    std::string ToString(size_t indent = 0) const override;

  private:
    uint32_t table_oid_;

    KeyId key_id_;

    std::vector<PlanLiteralValue> lookup_values_;

    std::vector<uint32_t> columns_;

    std::vector<SimplePredicate> residual_predicates_;
};

class PhysicalFilter final : public PhysicalPlan {
  public:
    PhysicalFilter(PlanExprPtr predicate, PhysicalPlanPtr child)
        : PhysicalPlan(Type::FILTER), predicate_(std::move(predicate)), child_(std::move(child)) {}

    const PlanExpr& GetPredicate() const {
        return *predicate_;
    }
    const PhysicalPlan& GetChild() const {
        return *child_;
    }
    PhysicalPlanPtr TakeChild() {
        return std::move(child_);
    }

    std::string ToString(size_t indent = 0) const override;

  private:
    PlanExprPtr predicate_;
    PhysicalPlanPtr child_;
};

class PhysicalProject final : public PhysicalPlan {
  public:
    PhysicalProject(std::vector<NamedPlanExpr> outputs, PhysicalPlanPtr child)
        : PhysicalPlan(Type::PROJECT), outputs_(std::move(outputs)), child_(std::move(child)) {}

    const std::vector<NamedPlanExpr>& GetOutputs() const {
        return outputs_;
    }
    const PhysicalPlan& GetChild() const {
        return *child_;
    }

    std::string ToString(size_t indent = 0) const override;

  private:
    std::vector<NamedPlanExpr> outputs_;
    PhysicalPlanPtr child_;
};

class PhysicalHashAggregate final : public PhysicalPlan {
  public:
    PhysicalHashAggregate(std::vector<PlanExprPtr> group_by, std::vector<NamedPlanExpr> outputs, PhysicalPlanPtr child)
        : PhysicalPlan(Type::HASH_AGGREGATE), group_by_(std::move(group_by)), outputs_(std::move(outputs)),
          child_(std::move(child)) {}

    const std::vector<PlanExprPtr>& GetGroupBy() const {
        return group_by_;
    }
    const std::vector<NamedPlanExpr>& GetOutputs() const {
        return outputs_;
    }
    const PhysicalPlan& GetChild() const {
        return *child_;
    }

    std::string ToString(size_t indent = 0) const override;

  private:
    std::vector<PlanExprPtr> group_by_;
    std::vector<NamedPlanExpr> outputs_;
    PhysicalPlanPtr child_;
};

class PhysicalInsert final : public PhysicalPlan {
  public:
    PhysicalInsert(uint32_t table_oid, std::vector<uint32_t> target_columns, std::vector<std::vector<PlanExprPtr>> rows)
        : PhysicalPlan(Type::INSERT), table_oid_(table_oid), target_columns_(std::move(target_columns)),
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

class PhysicalCreateTable final : public PhysicalPlan {
  public:
    PhysicalCreateTable(std::string table_name, TableSchema schema)
        : PhysicalPlan(Type::CREATE_TABLE), table_name_(std::move(table_name)), schema_(std::move(schema)) {}

    const std::string& GetTableName() const {
        return table_name_;
    }
    const TableSchema& GetSchema() const {
        return schema_;
    }

    std::string ToString(size_t indent = 0) const override;

  private:
    std::string table_name_;
    TableSchema schema_;
};

} // namespace simple_olap
