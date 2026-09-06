#include "aggregate.h"

#include <algorithm>
#include <cstring>
#include <limits>

namespace simple_olap {
AggregateOperator::AggregateOperator(std::unique_ptr<Operator> child, std::vector<AggFunction> agg_funcs)
    : child_(std::move(child)), agg_funcs_(std::move(agg_funcs)) {}

void AggregateOperator::Init() {
    child_->Init();

    // 每个聚合函数对应一个独立的中间状态
    states_.assign(agg_funcs_.size(), AggregateState{});

    consumed_ = false;
    result_emitted_ = false;
}

bool AggregateOperator::Next(VectorBatch& batch) {
    // Blocking 语义：第一次 Next() 时消费完子算子的全部数据
    if (!consumed_) {
        while (child_->Next(batch)) {
            total_row_count_ += batch.size;
            for (size_t a = 0; a < agg_funcs_.size(); ++a) {
                const auto& func = agg_funcs_[a];
                const auto& col = batch.columns[func.col_index];
                auto& st = states_[a];

                switch (func.type) {
                case AggFunction::Type::SUM: {
                    // 按列类型累加到 sum（double 累加器）
                    switch (col.type) {
                    case DataType::INT32: {
                        const auto* data = col.data<int32_t>();
                        for (uint32_t i = 0; i < batch.size; ++i) {
                            const uint32_t row_idx = batch.sel_vector[i];
                            st.sum += data[row_idx];
                        }
                        break;
                    }
                    case DataType::INT64: {
                        const auto* data = col.data<int64_t>();
                        for (uint32_t i = 0; i < batch.size; ++i) {
                            const uint32_t row_idx = batch.sel_vector[i];
                            st.sum += static_cast<double>(data[row_idx]);
                        }
                        break;
                    }
                    case DataType::FLOAT: {
                        const auto* data = col.data<float>();
                        for (uint32_t i = 0; i < batch.size; ++i) {
                            const uint32_t row_idx = batch.sel_vector[i];
                            st.sum += data[row_idx];
                        }
                        break;
                    }
                    case DataType::DOUBLE: {
                        const auto* data = col.data<double>();
                        for (uint32_t i = 0; i < batch.size; ++i) {
                            const uint32_t row_idx = batch.sel_vector[i];
                            st.sum += data[row_idx];
                        }
                        break;
                    }
                    default:
                        break;
                    }
                    break;
                }
                case AggFunction::Type::COUNT:
                    st.count += batch.size;
                    break;
                case AggFunction::Type::MIN: {
                    if (col.type == DataType::INT32) {
                        const auto* data = col.data<int32_t>();
                        for (uint32_t i = 0; i < batch.size; ++i) {
                            const uint32_t row_idx = batch.sel_vector[i];
                            st.min = st.has_min ? std::min(st.min, data[row_idx]) : data[row_idx];
                        }
                    } else if (col.type == DataType::INT64) {
                        const auto* data = col.data<int64_t>();
                        for (uint32_t i = 0; i < batch.size; ++i) {
                            const uint32_t row_idx = batch.sel_vector[i];
                            st.min = st.has_min ? std::min(st.min, static_cast<int32_t>(data[row_idx]))
                                                : static_cast<int32_t>(data[row_idx]);
                        }
                    }
                    st.has_min = st.has_min || col.count > 0;
                    break;
                }
                case AggFunction::Type::MAX: {
                    if (col.type == DataType::INT32) {
                        const auto* data = col.data<int32_t>();
                        for (uint32_t i = 0; i < batch.size; ++i) {
                            const uint32_t row_idx = batch.sel_vector[i];
                            st.max = st.has_max ? std::max(st.max, data[row_idx]) : data[row_idx];
                        }
                    } else if (col.type == DataType::INT64) {
                        const auto* data = col.data<int64_t>();
                        for (uint32_t i = 0; i < batch.size; ++i) {
                            const uint32_t row_idx = batch.sel_vector[i];
                            st.max = st.has_max ? std::max(st.max, static_cast<int32_t>(data[row_idx]))
                                                : static_cast<int32_t>(data[row_idx]);
                        }
                    }
                    st.has_max = st.has_max || col.count > 0;
                    break;
                }
                case AggFunction::Type::AVG:
                    // AVG = SUM / COUNT，sum 部分与 SUM 相同
                    break;
                }
            }
        }
        consumed_ = true;
    }

    // 只产出一行结果
    if (result_emitted_)
        return false;
    result_emitted_ = true;

    batch.Reset();
    for (size_t a = 0; a < agg_funcs_.size(); ++a) {
        const auto& func = agg_funcs_[a];
        const auto& st = states_[a];

        batch.AddColumn(DataType::INT64);
        auto& out_col = batch.columns.back();
        out_col.Resize(1);

        int64_t result = 0;
        switch (func.type) {
        case AggFunction::Type::SUM:
            result = static_cast<int64_t>(st.sum);
            break;
        case AggFunction::Type::COUNT:
            result = st.count;
            break;
        case AggFunction::Type::MIN:
            result = st.min;
            break;
        case AggFunction::Type::MAX:
            result = st.max;
            break;
        case AggFunction::Type::AVG:
            result = st.count > 0 ? static_cast<int64_t>(st.sum / static_cast<double>(st.count)) : 0;
            break;
        }
        std::memcpy(out_col.mutable_data<int64_t>(), &result, sizeof(int64_t));
    }
    batch.size = 1;
    batch.sel_vector = {0};

    return true;
}

double AggregateOperator::GetSumResult(size_t agg_idx) const {
    return states_.at(agg_idx).sum;
}

int64_t AggregateOperator::GetCountResult(size_t agg_idx) const {
    return states_.at(agg_idx).count;
}

double AggregateOperator::GetAvgResult(size_t agg_idx) const {
    const auto& st = states_.at(agg_idx);
    return st.count > 0 ? st.sum / static_cast<double>(st.count) : 0.0;
}

int32_t AggregateOperator::GetMinResult(size_t agg_idx) const {
    return states_.at(agg_idx).min;
}

int32_t AggregateOperator::GetMaxResult(size_t agg_idx) const {
    return states_.at(agg_idx).max;
}

// ==================== HashAggregateOperator ====================

// 分组键哈希：FNV-1a 变体，逐元素混合
size_t HashAggregateOperator::GroupKeyHash::operator()(const std::vector<int64_t>& key) const {
    size_t hash = 1469598103934665603ULL; // FNV offset basis
    for (const int64_t v : key) {
        hash ^= static_cast<size_t>(v);
        hash *= 1099511628211ULL; // FNV prime
    }
    return hash;
}

HashAggregateOperator::HashAggregateOperator(std::unique_ptr<Operator> child, AggregateSpec spec)
    : child_(std::move(child)), spec_(std::move(spec)) {}

void HashAggregateOperator::Init() {
    child_->Init();

    groups_.clear();
    group_order_.clear();
    group_types_.clear();

    consumed_ = false;
    emitted_ = false;
}

void HashAggregateOperator::ConsumeAll() {
    VectorBatch batch;
    while (child_->Next(batch)) {
        if (batch.size == 0) {
            continue;
        }

        // 第一次消费输入时记录分组列类型（用于构建输出 batch）
        if (group_types_.empty() && !spec_.group_by_slots.empty()) {
            group_types_.reserve(spec_.group_by_slots.size());
            for (const uint32_t slot : spec_.group_by_slots) {
                group_types_.push_back(batch.columns[slot].type);
            }
        }

        for (uint32_t i = 0; i < batch.size; ++i) {
            const uint32_t row_idx = batch.sel_vector[i];

            // 1. 提取该行的分组键
            std::vector<int64_t> key;
            key.reserve(spec_.group_by_slots.size());
            for (const uint32_t slot : spec_.group_by_slots) {
                const auto& col = batch.columns[slot];
                int64_t key_val = 0;
                switch (col.type) {
                case DataType::INT32:
                    key_val = static_cast<int64_t>(col.data<int32_t>()[row_idx]);
                    break;
                case DataType::INT64:
                    key_val = col.data<int64_t>()[row_idx];
                    break;
                case DataType::FLOAT:
                    key_val = static_cast<int64_t>(col.data<float>()[row_idx]);
                    break;
                case DataType::DOUBLE:
                    key_val = static_cast<int64_t>(col.data<double>()[row_idx]);
                    break;
                default:
                    break;
                }
                key.push_back(key_val);
            }

            // 2. 查找/插入分组状态
            auto [it, inserted] = groups_.try_emplace(std::move(key));
            if (inserted) {
                group_order_.push_back(&it->first);
            }
            AggState& st = it->second;

            // 3. 更新各聚合函数的中间状态
            for (const auto& agg : spec_.aggregates) {
                // COUNT(*)：无输入列，直接计数
                if (agg.type == AggType::COUNT && !agg.input_slot.has_value()) {
                    st.count += 1;
                    continue;
                }

                const auto& col = batch.columns[*agg.input_slot];
                const uint32_t r = row_idx;

                switch (agg.type) {
                case AggType::SUM:
                case AggType::AVG: // AVG = SUM / COUNT，先累加 sum
                {
                    switch (col.type) {
                    case DataType::INT32:
                        st.sum += static_cast<double>(col.data<int32_t>()[r]);
                        break;
                    case DataType::INT64:
                        st.sum += static_cast<double>(col.data<int64_t>()[r]);
                        break;
                    case DataType::FLOAT:
                        st.sum += static_cast<double>(col.data<float>()[r]);
                        break;
                    case DataType::DOUBLE:
                        st.sum += col.data<double>()[r];
                        break;
                    default:
                        break;
                    }
                    if (agg.type == AggType::SUM) {
                        st.count += 1; // SUM 也维护 count，便于统一输出
                    }
                    break;
                }
                case AggType::COUNT:
                    st.count += 1;
                    break;
                case AggType::MIN: {
                    double v = 0.0;
                    switch (col.type) {
                    case DataType::INT32:
                        v = static_cast<double>(col.data<int32_t>()[r]);
                        break;
                    case DataType::INT64:
                        v = static_cast<double>(col.data<int64_t>()[r]);
                        break;
                    case DataType::FLOAT:
                        v = static_cast<double>(col.data<float>()[r]);
                        break;
                    case DataType::DOUBLE:
                        v = col.data<double>()[r];
                        break;
                    default:
                        break;
                    }
                    st.min = st.has_min ? std::min(st.min, v) : v;
                    st.has_min = true;
                    break;
                }
                case AggType::MAX: {
                    double v = 0.0;
                    switch (col.type) {
                    case DataType::INT32:
                        v = static_cast<double>(col.data<int32_t>()[r]);
                        break;
                    case DataType::INT64:
                        v = static_cast<double>(col.data<int64_t>()[r]);
                        break;
                    case DataType::FLOAT:
                        v = static_cast<double>(col.data<float>()[r]);
                        break;
                    case DataType::DOUBLE:
                        v = col.data<double>()[r];
                        break;
                    default:
                        break;
                    }
                    st.max = st.has_max ? std::max(st.max, v) : v;
                    st.has_max = true;
                    break;
                }
                default:
                    break;
                }
            }
        }
    }
    consumed_ = true;
}

bool HashAggregateOperator::Next(VectorBatch& output) {
    // Blocking 语义：第一次 Next() 时消费完子算子的全部数据
    if (!consumed_) {
        ConsumeAll();
    }

    // 只产出一行结果（所有分组一次性输出）
    if (emitted_) {
        return false;
    }
    emitted_ = true;

    output.Reset();

    // 输出布局：先分组键列，再聚合结果列
    const size_t num_groups = spec_.group_by_slots.size();
    const size_t num_aggs = spec_.aggregates.size();

    for (const DataType t : group_types_) {
        output.AddColumn(t);
    }
    for (size_t a = 0; a < num_aggs; ++a) {
        output.AddColumn(DataType::INT64);
    }

    const size_t num_rows = group_order_.size();
    if (num_rows == 0) {
        output.size = 0;
        output.sel_vector.clear();
        return true;
    }

    for (auto& col : output.columns) {
        col.Resize(static_cast<uint32_t>(num_rows));
    }

    // 逐行填充：分组键 + 聚合结果
    for (size_t row = 0; row < num_rows; ++row) {
        const auto& key = *group_order_[row];
        const AggState& st = groups_.at(key);

        // 分组键列
        for (size_t g = 0; g < num_groups; ++g) {
            auto& col = output.columns[g];
            const int64_t key_val = key[g];
            switch (col.type) {
            case DataType::INT32:
                col.mutable_data<int32_t>()[row] = static_cast<int32_t>(key_val);
                break;
            case DataType::INT64:
                col.mutable_data<int64_t>()[row] = key_val;
                break;
            case DataType::FLOAT:
                col.mutable_data<float>()[row] = static_cast<float>(key_val);
                break;
            case DataType::DOUBLE:
                col.mutable_data<double>()[row] = static_cast<double>(key_val);
                break;
            default:
                break;
            }
        }

        // 聚合结果列
        for (size_t a = 0; a < num_aggs; ++a) {
            const auto& agg = spec_.aggregates[a];
            auto& col = output.columns[num_groups + a];

            int64_t result = 0;
            switch (agg.type) {
            case AggType::SUM:
                result = static_cast<int64_t>(st.sum);
                break;
            case AggType::COUNT:
                result = st.count;
                break;
            case AggType::AVG:
                result = st.count > 0 ? static_cast<int64_t>(st.sum / static_cast<double>(st.count)) : 0;
                break;
            case AggType::MIN:
                result = static_cast<int64_t>(st.min);
                break;
            case AggType::MAX:
                result = static_cast<int64_t>(st.max);
                break;
            default:
                break;
            }
            col.mutable_data<int64_t>()[row] = result;
        }
    }

    output.size = static_cast<uint32_t>(num_rows);
    output.sel_vector.resize(num_rows);
    for (uint32_t i = 0; i < output.size; ++i) {
        output.sel_vector[i] = i;
    }

    return true;
}

} // namespace simple_olap
