#include "aggregate.h"

#include <algorithm>
#include <cstring>
#include <limits>

namespace simple_olap
{
    AggregateOperator::AggregateOperator(std::unique_ptr<Operator> child,
                                         std::vector<AggFunction> agg_funcs)
        : child_(std::move(child)), agg_funcs_(std::move(agg_funcs))
    {
    }

    void AggregateOperator::Init()
    {
        child_->Init();

        // 每个聚合函数对应一个独立的中间状态
        states_.assign(agg_funcs_.size(), AggregateState{});

        consumed_ = false;
        result_emitted_ = false;
    }

    bool AggregateOperator::Next(VectorBatch &batch)
    {
        // Blocking 语义：第一次 Next() 时消费完子算子的全部数据
        if (!consumed_)
        {
            while (child_->Next(batch))
            {
                total_row_count_ += batch.size;
                for (size_t a = 0; a < agg_funcs_.size(); ++a)
                {
                    const auto &func = agg_funcs_[a];
                    const auto &col = batch.columns[func.col_index];
                    auto &st = states_[a];

                    switch (func.type)
                    {
                    case AggFunction::Type::SUM:
                    {
                        // 按列类型累加到 sum（double 累加器）
                        switch (col.type)
                        {
                        case DataType::INT32:
                        {
                            const auto *data = col.data<int32_t>();
                            for (uint32_t i = 0; i < col.count; ++i)
                                st.sum += data[i];
                            break;
                        }
                        case DataType::INT64:
                        {
                            const auto *data = col.data<int64_t>();
                            for (uint32_t i = 0; i < col.count; ++i)
                                st.sum += static_cast<double>(data[i]);
                            break;
                        }
                        case DataType::FLOAT:
                        {
                            const auto *data = col.data<float>();
                            for (uint32_t i = 0; i < col.count; ++i)
                                st.sum += data[i];
                            break;
                        }
                        case DataType::DOUBLE:
                        {
                            const auto *data = col.data<double>();
                            for (uint32_t i = 0; i < col.count; ++i)
                                st.sum += data[i];
                            break;
                        }
                        default:
                            break;
                        }
                        break;
                    }
                    case AggFunction::Type::COUNT:
                        st.count += col.count;
                        break;
                    case AggFunction::Type::MIN:
                    {
                        if (col.type == DataType::INT32)
                        {
                            const auto *data = col.data<int32_t>();
                            for (uint32_t i = 0; i < col.count; ++i)
                                st.min = st.has_min ? std::min(st.min, data[i]) : data[i];
                        }
                        else if (col.type == DataType::INT64)
                        {
                            const auto *data = col.data<int64_t>();
                            for (uint32_t i = 0; i < col.count; ++i)
                                st.min = st.has_min ? std::min(st.min, static_cast<int32_t>(data[i]))
                                                    : static_cast<int32_t>(data[i]);
                        }
                        st.has_min = st.has_min || col.count > 0;
                        break;
                    }
                    case AggFunction::Type::MAX:
                    {
                        if (col.type == DataType::INT32)
                        {
                            const auto *data = col.data<int32_t>();
                            for (uint32_t i = 0; i < col.count; ++i)
                                st.max = st.has_max ? std::max(st.max, data[i]) : data[i];
                        }
                        else if (col.type == DataType::INT64)
                        {
                            const auto *data = col.data<int64_t>();
                            for (uint32_t i = 0; i < col.count; ++i)
                                st.max = st.has_max ? std::max(st.max, static_cast<int32_t>(data[i]))
                                                    : static_cast<int32_t>(data[i]);
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
        for (size_t a = 0; a < agg_funcs_.size(); ++a)
        {
            const auto &func = agg_funcs_[a];
            const auto &st = states_[a];

            batch.AddColumn(DataType::INT64);
            auto &out_col = batch.columns.back();
            out_col.Resize(1);

            int64_t result = 0;
            switch (func.type)
            {
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
                result = st.count > 0
                             ? static_cast<int64_t>(st.sum / static_cast<double>(st.count))
                             : 0;
                break;
            }
            std::memcpy(out_col.mutable_data<int64_t>(), &result, sizeof(int64_t));
        }
        batch.size = 1;
        batch.sel_vector = {0};

        return true;
    }

    double AggregateOperator::GetSumResult(size_t agg_idx) const
    {
        return states_.at(agg_idx).sum;
    }

    int64_t AggregateOperator::GetCountResult(size_t agg_idx) const
    {
        return states_.at(agg_idx).count;
    }

    double AggregateOperator::GetAvgResult(size_t agg_idx) const
    {
        const auto &st = states_.at(agg_idx);
        return st.count > 0 ? st.sum / static_cast<double>(st.count) : 0.0;
    }

    int32_t AggregateOperator::GetMinResult(size_t agg_idx) const
    {
        return states_.at(agg_idx).min;
    }

    int32_t AggregateOperator::GetMaxResult(size_t agg_idx) const
    {
        return states_.at(agg_idx).max;
    }

} // namespace simple_olap
