#include "hash_aggregate.h"

#include <functional>
#include <stdexcept>
#include <type_traits>

#include "../batch_utils.h"

namespace simple_olap {

namespace {

size_t HashCombine(size_t seed, size_t value) {
    return seed ^ (value + 0x9e3779b97f4a7c15ULL + (seed << 6U) + (seed >> 2U));
}

size_t HashExecValue(const ExecValue& value) {
    size_t seed = value.index();
    return std::visit(
        [seed](const auto& v) mutable -> size_t {
            using T = std::decay_t<decltype(v)>;
            return HashCombine(seed, std::hash<T>{}(v));
        },
        value);
}

} // namespace

size_t HashAggregateOperator::GroupKeyHash::operator()(const GroupKey& key) const {
    size_t seed = 0;
    for (const auto& value : key.values) {
        seed = HashCombine(seed, HashExecValue(value));
    }
    return seed;
}

HashAggregateOperator::StateVector HashAggregateOperator::MakeStates() const {
    return StateVector(agg_calls_.size());
}

HashAggregateOperator::GroupKey HashAggregateOperator::EvalGroupKey(const VectorBatch& batch,
                                                                    uint32_t physical_row) const {
    GroupKey key;
    key.values.reserve(group_exprs_.size());
    for (const auto& expr : group_exprs_) {
        key.values.push_back(expr->Eval(batch, physical_row));
    }
    return key;
}

void HashAggregateOperator::UpdateAggregate(const AggCallSpec& call, AggState& state, const VectorBatch& batch,
                                            uint32_t physical_row) {
    if (call.type == AggType::COUNT) {
        // v1 has no NULL bitmap. Therefore COUNT(col) and COUNT(*) differ in IR,
        // but both count active rows until NULL support is added.
        if (call.arg) {
            (void)call.arg->Eval(batch, physical_row);
        }
        ++state.count;
        return;
    }

    if (!call.arg) {
        throw std::runtime_error("HashAggregateOperator: aggregate argument is missing");
    }

    const ExecValue value = call.arg->Eval(batch, physical_row);

    switch (call.type) {
    case AggType::SUM:
        state.sum += ExecValueAsNumber(value);
        state.has_value = true;
        break;
    case AggType::AVG:
        state.sum += ExecValueAsNumber(value);
        ++state.count;
        state.has_value = true;
        break;
    case AggType::MIN:
        if (!state.has_value || CompareExecValues(value, state.min_value) < 0) {
            state.min_value = value;
        }
        state.has_value = true;
        break;
    case AggType::MAX:
        if (!state.has_value || CompareExecValues(value, state.max_value) > 0) {
            state.max_value = value;
        }
        state.has_value = true;
        break;
    default:
        throw std::runtime_error("HashAggregateOperator: unsupported aggregate type");
    }
}

ExecValue HashAggregateOperator::FinalizeAggregate(const AggCallSpec& call, const AggState& state) const {
    switch (call.type) {
    case AggType::COUNT:
        return static_cast<int64_t>(state.count);
    case AggType::SUM:
        return CastNumber(state.sum, call.result_type);
    case AggType::AVG:
        return static_cast<double>(state.count == 0 ? 0.0L : state.sum / static_cast<long double>(state.count));
    case AggType::MIN:
        return state.has_value ? state.min_value : CastNumber(0.0L, call.result_type);
    case AggType::MAX:
        return state.has_value ? state.max_value : CastNumber(0.0L, call.result_type);
    default:
        throw std::runtime_error("HashAggregateOperator: unsupported aggregate type");
    }
}

void HashAggregateOperator::MergeAggState(const AggCallSpec& call, AggState& dst, const AggState& src) const {
    switch (call.type) {
    case AggType::COUNT:
        // COUNT 的中间态就是行数，直接相加
        dst.count += src.count;
        break;
    case AggType::SUM:
        dst.sum += src.sum;
        dst.has_value = dst.has_value || src.has_value;
        break;
    case AggType::AVG:
        // AVG 的中间态是 (sum, count)，合并后再 finalize，语义正确
        dst.sum += src.sum;
        dst.count += src.count;
        dst.has_value = dst.has_value || src.has_value;
        break;
    case AggType::MIN:
        if (src.has_value && (!dst.has_value || CompareExecValues(src.min_value, dst.min_value) < 0)) {
            dst.min_value = src.min_value;
        }
        dst.has_value = dst.has_value || src.has_value;
        break;
    case AggType::MAX:
        if (src.has_value && (!dst.has_value || CompareExecValues(src.max_value, dst.max_value) > 0)) {
            dst.max_value = src.max_value;
        }
        dst.has_value = dst.has_value || src.has_value;
        break;
    default:
        throw std::runtime_error("HashAggregateOperator: unsupported aggregate type");
    }
}

void HashAggregateOperator::MergeGroups(GroupTable other) {
    for (auto& [key, states] : other) {
        auto [it, inserted] = groups_.try_emplace(std::move(key), MakeStates());
        auto& dst_states = it->second;
        for (uint32_t i = 0; i < static_cast<uint32_t>(agg_calls_.size()); ++i) {
            MergeAggState(agg_calls_[i], dst_states[i], states[i]);
        }
    }
}

void HashAggregateOperator::ComputePartialGroups() {
    if (group_exprs_.empty()) {
        // 无 GROUP BY：全局聚合，每个 worker 仍要产出一个（可能为空的）全局组，
        // 保证空输入时全局阶段也能得到一个空结果组。
        groups_.try_emplace(GroupKey{}, MakeStates());
    }

    while (child_->Next(input_)) {
        const uint32_t active = ActiveRowCount(input_);
        for (uint32_t logical = 0; logical < active; ++logical) {
            const uint32_t physical = ActiveRowIndex(input_, logical);
            GroupKey key = EvalGroupKey(input_, physical);
            auto [it, inserted] = groups_.try_emplace(std::move(key), MakeStates());
            auto& states = it->second;
            for (uint32_t i = 0; i < static_cast<uint32_t>(agg_calls_.size()); ++i) {
                UpdateAggregate(agg_calls_[i], states[i], input_, physical);
            }
        }
    }
}

void HashAggregateOperator::ConsumeAll() {
    ComputePartialGroups();
}

void HashAggregateOperator::Init() {
    child_->Init();
    ClearBatch(input_);
    groups_.clear();
    consumed_ = false;
    external_groups_ = false;
}

bool HashAggregateOperator::Next(VectorBatch& output) {
    if (!consumed_) {
        if (!external_groups_) {
            ConsumeAll();
        }
        emit_it_ = groups_.begin();
        consumed_ = true;
    }

    if (emit_it_ == groups_.end()) {
        ClearBatch(output);
        return false;
    }

    std::vector<const GroupTable::value_type*> rows;
    rows.reserve(VectorBatch::BATCH_SIZE);
    while (emit_it_ != groups_.end() && rows.size() < VectorBatch::BATCH_SIZE) {
        rows.push_back(&*emit_it_);
        ++emit_it_;
    }

    const uint32_t row_count = static_cast<uint32_t>(rows.size());
    ClearBatch(output);
    for (const auto& spec : outputs_) {
        output.AddColumn(spec.type);
        output.columns.back().Resize(row_count);
    }

    for (uint32_t row = 0; row < row_count; ++row) {
        const GroupKey& key = rows[row]->first;
        const StateVector& states = rows[row]->second;

        for (uint32_t col = 0; col < static_cast<uint32_t>(outputs_.size()); ++col) {
            const auto& spec = outputs_[col];
            ExecValue value;
            if (spec.kind == AggregateOutputSpec::Kind::GROUP_KEY) {
                value = key.values.at(spec.index);
            } else {
                value = FinalizeAggregate(agg_calls_.at(spec.index), states.at(spec.index));
            }
            WriteExecValue(output.columns[col], row, value);
        }
    }

    output.size = row_count;
    output.sel_vector.clear();
    return row_count > 0;
}

} // namespace simple_olap
