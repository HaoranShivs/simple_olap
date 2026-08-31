#include "columnchunk.h"

#include <algorithm>
#include <cstring>

#include "../file/filewriter.h"

namespace simple_olap
{

    // ==========================================
    // ColumnBuilder - 列数据写入器
    // ==========================================

    ColumnBuilder::ColumnBuilder(const ColumnChunkMeta &metadata)
        : metadata_(metadata), row_count_(0)
    {
        // 初始为无统计信息；首次 Append 时以第一个值初始化 min/max
        metadata_.has_stats = false;
        metadata_.min_value = 0.0;
        metadata_.max_value = 0.0;
    }

    void ColumnBuilder::Append(const ColumnVector &column)
    {
        const size_t byte_count = column.element_size * column.row_count;

        // 动态增长缓冲，把新数据追加到末尾
        const size_t old_size = data_.size();
        data_.resize(old_size + byte_count);
        std::memcpy(data_.data() + old_size, column.data, byte_count);

        // 更新 min/max 统计（仅数值列；VARCHAR 等无法比较的列跳过）
        // 以 double 统一存储，INT64 超过 2^53 时存在精度损失，暂可接受
        switch (metadata_.type)
        {
        case DataType::INT32:
        case DataType::INT64:
        case DataType::FLOAT:
        case DataType::DOUBLE:
        {
            const uint8_t *ptr = column.data;
            for (uint32_t i = 0; i < column.row_count; ++i)
            {
                double v = 0.0;
                switch (metadata_.type)
                {
                case DataType::INT32:
                    v = static_cast<double>(*reinterpret_cast<const int32_t *>(ptr));
                    break;
                case DataType::INT64:
                    v = static_cast<double>(*reinterpret_cast<const int64_t *>(ptr));
                    break;
                case DataType::FLOAT:
                    v = static_cast<double>(*reinterpret_cast<const float *>(ptr));
                    break;
                case DataType::DOUBLE:
                    v = *reinterpret_cast<const double *>(ptr);
                    break;
                default:
                    break;
                }
                if (!metadata_.has_stats)
                {
                    metadata_.min_value = v;
                    metadata_.max_value = v;
                    metadata_.has_stats = true;
                }
                else
                {
                    metadata_.min_value = std::min(metadata_.min_value, v);
                    metadata_.max_value = std::max(metadata_.max_value, v);
                }
                ptr += column.element_size;
            }
            break;
        }
        default:
            // VARCHAR / INVALID：不维护统计信息
            break;
        }

        row_count_ += column.row_count;
    }

    // 把已积累的列数据写入 writer 当前位置，返回是否成功
    bool ColumnBuilder::Flush(FileWriter &writer)
    {
        if (data_.empty())
        {
            return true;
        }
        return writer.Write(data_.data(), data_.size());
    }

} // namespace simple_olap
