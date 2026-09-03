#include "storage_manager.h"

#include <algorithm>
#include <memory>

#include "../../execution/vector/vector.h"

namespace simple_olap
{
    namespace
    {
        // 按行读取列值并转为 double，供 where 行级过滤比较；非数值类型返回 false
        bool ColumnValueAsDouble(const ColumnData &col, uint32_t row, double &out)
        {
            switch (col.type)
            {
            case DataType::INT32:
                out = static_cast<double>(col.data<int32_t>()[row]);
                return true;
            case DataType::INT64:
                out = static_cast<double>(col.data<int64_t>()[row]);
                return true;
            case DataType::FLOAT:
                out = static_cast<double>(col.data<float>()[row]);
                return true;
            case DataType::DOUBLE:
                out = col.data<double>()[row];
                return true;
            default:
                return false;
            }
        }

        // 标量比较：lhs op rhs
        bool CompareDouble(double lhs, CmpOp op, double rhs)
        {
            switch (op)
            {
            case CmpOp::EQ: return lhs == rhs;
            case CmpOp::NE: return lhs != rhs;
            case CmpOp::GT: return lhs > rhs;
            case CmpOp::GE: return lhs >= rhs;
            case CmpOp::LT: return lhs < rhs;
            case CmpOp::LE: return lhs <= rhs;
            }
            return false;
        }

        // 从 Condition 的 variant 值中提取 double（仅数值；字符串返回 false）
        bool ColumnValueAsDoubleFromVariant(
            const std::variant<int32_t, int64_t, double, std::string> &value, double &out)
        {
            if (std::holds_alternative<int32_t>(value))
            {
                out = static_cast<double>(std::get<int32_t>(value));
                return true;
            }
            if (std::holds_alternative<int64_t>(value))
            {
                out = static_cast<double>(std::get<int64_t>(value));
                return true;
            }
            if (std::holds_alternative<double>(value))
            {
                out = std::get<double>(value);
                return true;
            }
            return false;
        }
    } // namespace
    StorageManager::StorageManager(TableId table_id, std::filesystem::path path, const TableMeta &metadata)
        : table_meta_(metadata),
          table_path_(std::move(path))
    {
        // 以传入的 table_id 为准（防止上层拷贝时不一致）
        table_meta_.table_id = table_id;

        // 新 segment 的 id 从已落盘最大 id + 1 开始
        SegmentId next = 0;
        for (SegmentId id : table_meta_.segment_ids)
        {
            next = std::max(next, id + 1);
        }
        next_segment_id_ = next;
        CreateActiveSegment();
    }

    void StorageManager::Append(const DataChunk &input)
    {
        // 浅拷贝，共享底层缓冲；后续用 Slice 切块
        DataChunk chunk = input;
        while (chunk.size() > 0)
        {
            // 活跃 segment 的剩余容量
            uint32_t remaining = kMaxSegmentRowCount - active_segment_->row_count();

            if (chunk.size() <= remaining)
            {
                // 整个 chunk 放得下，直接写入
                active_segment_->Append(chunk);
                break;
            }

            // 剩余容量不够，切块：本 segment 先装前 remaining 行
            DataChunk rest = chunk.Slice(remaining);
            active_segment_->Append(chunk);

            // 封存当前 segment，开启新的活跃 segment
            SealActiveSegment();
            CreateActiveSegment();

            chunk = std::move(rest);
        }
    }

    void StorageManager::SealActiveSegment()
    {
        // 活跃 segment 已填满：不立即写盘，移入待刷盘 map，等待 Flush() 统一落盘
        sealed_segments_[active_segment_id_] = std::move(active_segment_);
    }

    StorageManager::~StorageManager()
    {
        // 析构兜底：把非空的活跃 segment 封存并落盘，防止进程退出丢数据。
        // 注意：这里只负责数据落盘，TableMeta 的持久化（table.meta）由上层 Table 负责。
        if (active_segment_ != nullptr && active_segment_->row_count() > 0)
        {
            SealActiveSegment();
            CreateActiveSegment();
        }
        if (!sealed_segments_.empty())
        {
            Flush();
        }
    }

    void StorageManager::Flush()
    {
        // 将内存中所有待刷盘 segment 写盘，并把 id 直接登记进 TableMeta。
        // 文件命名格式固定：table_path_ / {segment_id}，与 SegmentBuilder::Flush /
        // SegmentReader::Open 的约定一致。
        for (auto &entry : sealed_segments_)
        {
            const std::filesystem::path seg_path =
                table_path_ / std::to_string(entry.first);
            if (entry.second->Flush(seg_path))
            {
                table_meta_.segment_ids.push_back(entry.first);
            }
        }
        sealed_segments_.clear();
    }

    void StorageManager::CreateActiveSegment()
    {
        active_segment_id_ = next_segment_id_++;
        active_segment_ = std::make_unique<SegmentBuilder>(table_meta_.schema);
    }

    SegmentReader *StorageManager::GetSegmentReader(SegmentId id)
    {
        // 懒加载：首次访问时 mmap 打开并缓存，后续复用
        auto it = reader_cache_.find(id);
        if (it != reader_cache_.end())
        {
            return it->second.get();
        }

        SegmentReader *reader = SegmentReader::Open(id, table_path_);
        if (reader == nullptr)
        {
            return nullptr;
        }
        reader_cache_.emplace(id, std::unique_ptr<SegmentReader>(reader));
        return reader_cache_[id].get();
    }

    bool StorageManager::GetVectorBatch(const SelectTargetStatement &request,
                                        ScanCursor &cursor, VectorBatch &output)
    {
        // ---------- 组装 ScanOptions，衔接 SegmentReader::GetVectorBatch ----------
        // target_list 的列即需要扫描的列；where 条件同时用于
        // segment 级 min/max 剪枝（在 SegmentReader 内 offset==0 时检查）
        // 与这里的行级过滤。
        ScanOptions options;
        options.columns.reserve(request.target_list.size() + 1);
        for (const auto &target : request.target_list)
        {
            options.columns.push_back(target.column_id);
        }

        if (request.where)
        {
            options.has_where = true;
            options.cond.column = request.where->column_id;
            options.cond.op = request.where->op;
            options.cond.value = request.where->value; // int32_t

            // where 列可能不在输出列中，但行级过滤需要它的数据，
            // 追加到扫描列末尾，过滤完成后移除
            if (std::find(options.columns.begin(), options.columns.end(),
                          options.cond.column) == options.columns.end())
            {
                options.columns.push_back(options.cond.column);
            }
        }

        const size_t output_col_count = request.target_list.size();

        // ---------- 跨 segment 推进游标 ----------
        const auto &segment_ids = table_meta_.segment_ids;
        while (cursor.segment_id < segment_ids.size())
        {
            const SegmentId seg_id = segment_ids[cursor.segment_id];
            SegmentReader *reader = GetSegmentReader(seg_id);
            if (reader == nullptr)
            {
                // 打开失败：跳过该 segment，继续尝试下一个
                cursor.segment_id += 1;
                cursor.offset_in_segment = 0;
                continue;
            }

            // 从当前 segment 的 offset_in_segment 处扫描一批（最多 1024 行）；
            // 返回 false 表示本 segment 无数据（读完或被 min/max 剪枝）
            const bool scanned =
                reader->GetVectorBatch(options, cursor.offset_in_segment, output);

            if (!scanned || output.size == 0)
            {
                cursor.segment_id += 1;
                cursor.offset_in_segment = 0;
                continue;
            }

            // 有数据：游标在本 segment 内前移
            cursor.offset_in_segment += output.size;

            // ---------- 行级过滤 ----------
            if (options.has_where)
            {
                // 定位 where 列在 output 中的下标
                const auto where_it = std::find(options.columns.begin(),
                                                options.columns.end(),
                                                options.cond.column);
                const size_t where_idx =
                    static_cast<size_t>(where_it - options.columns.begin());

                double rhs = 0.0;
                const bool rhs_ok = ColumnValueAsDoubleFromVariant(options.cond.value, rhs);

                output.sel_vector.clear();
                const auto &where_col = output.columns[where_idx];
                for (uint32_t row = 0; row < output.size; ++row)
                {
                    double lhs = 0.0;
                    if (!rhs_ok || !ColumnValueAsDouble(where_col, row, lhs))
                    {
                        // 无法比较（字符串等）：保守保留该行
                        output.sel_vector.push_back(row);
                        continue;
                    }
                    if (CompareDouble(lhs, options.cond.op, rhs))
                    {
                        output.sel_vector.push_back(row);
                    }
                }

                // // 按 sel_vector 压缩各列（如需物化可调用；当前保留视图 + sel_vector）
                // output.CompactBySel();

                // 移除为过滤而追加的 where 列
                if (output.columns.size() > output_col_count)
                {
                    output.columns.resize(output_col_count);
                }

                if (output.sel_vector.empty())
                {
                    // 本批全部被过滤：继续循环读下一批
                    continue;
                }
            }
            else
            {
                output.sel_vector.clear();
            }

            return true;
        }

        // 所有 segment 都已读完
        output.Reset();
        return false;
    }

} // namespace simple_olap
