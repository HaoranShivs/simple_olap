#include "segment.h"

#include <algorithm>
#include <cstring>
#include <stdexcept>

#include "../../execution/vector/vector.h"
#include "../file/filewriter.h"

namespace simple_olap {
// ==========================================
// SegmentReader - 基于 mmap 的只读 segment
// ==========================================
// 磁盘文件格式：
//   [SegmentMeta 序列化区][列0数据][列1数据]...
// Open 时将整个文件 mmap 进内存，元数据从头部反序列化，
// 各列的 ColumnChunkReader 直接指向映射区域内的 data_offset 处，零拷贝。

SegmentReader::SegmentReader(uint64_t segment_id, SegmentMeta metadata, MappedFile file)
    : segment_id_(segment_id), metadata_(std::move(metadata)), mapped_file_(std::move(file)) {}

SegmentReader::~SegmentReader() = default;

SegmentReader* SegmentReader::Open(SegmentId id, const std::filesystem::path& root_dir) {
    // 1. 定位 segment 文件（命名格式固定：root_dir / {id}）
    const std::filesystem::path seg_path = root_dir / std::to_string(id);

    // 2. mmap 只读映射整个文件
    auto mapped = MappedFile::OpenReadOnly(seg_path);
    if (!mapped.has_value()) {
        return nullptr;
    }

    // 3. 从映射区头部反序列化 SegmentMeta
    SegmentMeta meta;
    try {
        BinaryReader reader(reinterpret_cast<const uint8_t*>(mapped->data()), mapped->size());
        meta = SegmentMeta::Deserialize(reader);
    } catch (const std::exception&) {
        // 元数据损坏或文件不完整
        return nullptr;
    }

    if (meta.segment_id != id) {
        // 文件内容与请求的 id 不一致
        return nullptr;
    }

    // 4. 构造 reader，并为每列建立指向映射区的 ColumnChunkReader 视图（零拷贝）
    auto* self = new SegmentReader(id, std::move(meta), std::move(*mapped));

    const std::byte* base = self->mapped_file_.data();
    for (const auto& chunk_meta : self->metadata_.col_chunk_metas_) {
        const uint8_t* col_data = reinterpret_cast<const uint8_t*>(base + chunk_meta.data_offset);
        self->columns_.emplace(chunk_meta.column_id,
                               ColumnChunkReader::CreateFromBuilder(chunk_meta, col_data, self->metadata_.row_count,
                                                                    TypeElemSize(chunk_meta.type)));
    }

    return self;
}

uint64_t SegmentReader::id() const {
    return segment_id_;
}

uint32_t SegmentReader::row_count() const {
    return metadata_.row_count;
}

const ColumnChunkMeta& SegmentReader::GetColumnMeta(ColumnId id) const noexcept {
    for (const auto& chunk : metadata_.col_chunk_metas_) {
        if (chunk.column_id == id) {
            return chunk;
        }
    }
    // 未找到：返回首个作为兜底（调用方应保证 id 有效）
    return metadata_.col_chunk_metas_.front();
}

ColumnChunkReader SegmentReader::OpenColumn(uint32_t column_id) const {
    auto it = columns_.find(column_id);
    if (it != columns_.end()) {
        return it->second;
    }
    // 未找到列：返回空视图
    return ColumnChunkReader::CreateFromBuilder(ColumnChunkMeta{}, nullptr, 0, 0);
}

// 从 Condition 的 variant 值中提取 double（仅数值比较；字符串返回 false）
static bool CondValueAsDouble(const Condition& cond, double& out) {
    if (std::holds_alternative<int32_t>(cond.value)) {
        out = static_cast<double>(std::get<int32_t>(cond.value));
        return true;
    }
    if (std::holds_alternative<int64_t>(cond.value)) {
        out = static_cast<double>(std::get<int64_t>(cond.value));
        return true;
    }
    if (std::holds_alternative<double>(cond.value)) {
        out = std::get<double>(cond.value);
        return true;
    }
    return false; // VARCHAR 条件无法用 min/max 剪枝
}

// segment 级 min/max 三态判断：根据列统计信息判断单个 predicate 与整个 segment 的关系。
//   SKIP        -> 整个 segment 无行能满足条件，可安全跳过
//   ALL_MATCH   -> 该 predicate 对 segment 内所有行恒成立，行级无需再判断
//   NEED_FILTER -> metadata 无法决定，需要行级精确过滤
static SegmentMatch EvaluateByStats(const ColumnChunkMeta& meta, const Condition& cond) {
    // 没有统计信息：不能 prune，也不能证明 all-match
    if (!meta.has_stats) {
        return SegmentMatch::NEED_FILTER;
    }

    double value = 0.0;
    if (!CondValueAsDouble(cond, value)) {
        return SegmentMatch::NEED_FILTER;
    }

    const double min = meta.min_value;
    const double max = meta.max_value;

    switch (cond.op) {
    case CmpOp::EQ:
        if (value < min || value > max) {
            return SegmentMatch::SKIP;
        }
        if (min == value && max == value) {
            return SegmentMatch::ALL_MATCH;
        }
        return SegmentMatch::NEED_FILTER;

    case CmpOp::NE:
        if (min == value && max == value) {
            return SegmentMatch::SKIP;
        }
        if (value < min || value > max) {
            return SegmentMatch::ALL_MATCH;
        }
        return SegmentMatch::NEED_FILTER;

    case CmpOp::GT:
        if (max <= value) {
            return SegmentMatch::SKIP;
        }
        if (min > value) {
            return SegmentMatch::ALL_MATCH;
        }
        return SegmentMatch::NEED_FILTER;

    case CmpOp::GE:
        if (max < value) {
            return SegmentMatch::SKIP;
        }
        if (min >= value) {
            return SegmentMatch::ALL_MATCH;
        }
        return SegmentMatch::NEED_FILTER;

    case CmpOp::LT:
        if (min >= value) {
            return SegmentMatch::SKIP;
        }
        if (max < value) {
            return SegmentMatch::ALL_MATCH;
        }
        return SegmentMatch::NEED_FILTER;

    case CmpOp::LE:
        if (min > value) {
            return SegmentMatch::SKIP;
        }
        if (max <= value) {
            return SegmentMatch::ALL_MATCH;
        }
        return SegmentMatch::NEED_FILTER;
    }

    return SegmentMatch::NEED_FILTER;
}

SegmentFilterDecision SegmentReader::EvaluatePredicates(const std::vector<Condition>& predicates) const {
    SegmentFilterDecision decision;

    decision.row_filter_mask.resize(predicates.size(), 0);

    for (size_t i = 0; i < predicates.size(); ++i) {
        const Condition& cond = predicates[i];

        const auto& meta = GetColumnMeta(cond.column);

        const SegmentMatch match = EvaluateByStats(meta, cond);

        // AND 语义：
        // 任意 predicate 不可能满足，整个 segment 都不能满足。
        if (match == SegmentMatch::SKIP) {
            decision.skip_segment = true;
            decision.row_filter_mask.clear();
            return decision;
        }

        if (match == SegmentMatch::NEED_FILTER) {
            decision.row_filter_mask[i] = 1;
        }
    }

    return decision;
}

bool SegmentReader::GetVectorBatch(const ScanOptions& scanoptions, uint32_t offset, VectorBatch& output) {
    // ============ 批量扫描 ============
    //
    // 注意：本方法不再负责 predicate 语义。
    // metadata 判断（EvaluatePredicates）与行级过滤（ApplyRowPredicates）
    // 由 StorageManager 统一编排，SegmentReader 只负责：
    //   metadata access + column access + batch scan。
    const uint32_t segment_rows = metadata_.row_count;
    if (offset >= segment_rows) {
        // 已越过 segment 末尾，无数据可读
        return false;
    }

    // 本次扫描行数：从 offset 起最多 VectorBatch::BATCH_SIZE（1024）行
    const uint32_t scan_count = std::min(static_cast<uint32_t>(VectorBatch::BATCH_SIZE), segment_rows - offset);

    // 待输出的列集合：scanoptions.columns 为空表示输出全部列
    std::vector<ColumnId> out_columns;
    if (scanoptions.columns.empty()) {
        out_columns.reserve(metadata_.col_chunk_metas_.size());
        for (const auto& chunk_meta : metadata_.col_chunk_metas_) {
            out_columns.push_back(chunk_meta.column_id);
        }
    } else {
        out_columns = scanoptions.columns;
    }

    // 重建 output 的列结构（列顺序与 out_columns 一致）
    output.columns.clear();
    output.sel_vector.clear();
    output.size = 0;
    for (ColumnId col_id : out_columns) {
        output.AddColumn(GetColumnMeta(col_id).type);
    }

    // 逐列从列块中拷贝 [offset, offset + scan_count) 行
    for (size_t i = 0; i < out_columns.size(); ++i) {
        const ColumnChunkReader chunk = OpenColumn(out_columns[i]);
        const ColumnChunkMeta& chunk_meta = chunk.metadata();
        const size_t elem_size = TypeElemSize(chunk_meta.type);

        // 列块数据在映射区内连续，直接定位到起始行后整段拷贝
        const std::byte* col_start = chunk.data() + static_cast<uint64_t>(offset) * elem_size;

        auto& col_data = output.columns[i];
        col_data.type = chunk_meta.type;
        // col_data.bit_map.clear();
        col_data.CopyFrom(col_start, scan_count, true);
    }

    output.size = scan_count;
    return scan_count > 0;
}

// ==========================================
// SegmentBuilder - 写入侧
// ==========================================

SegmentBuilder::SegmentBuilder(const TableSchema& schema) : row_count_(0) {
    // 为 schema 中的每一列创建一个 ColumnBuilder
    column_builders_.reserve(schema.columns.size());
    for (const auto& col : schema.columns) {
        ColumnChunkMeta chunk_meta;
        chunk_meta.column_id = col.column_id;
        chunk_meta.type = col.type;
        chunk_meta.data_offset = 0;
        column_builders_.emplace_back(chunk_meta);
    }
}

void SegmentBuilder::Append(const DataChunk& batch) {
    const uint32_t row_count = static_cast<uint32_t>(batch.size());

    // 为每一列构造 ColumnVector 视图，交给对应的 ColumnBuilder
    for (size_t i = 0; i < column_builders_.size(); ++i) {
        ColumnVector vector;
        // 其实可以直接转成目标类型，不过为了解决 datachunk 中 slince 方法的问题，这里还是保持 unint8 了
        vector.data = batch.data<uint8_t>(i);
        vector.element_size = batch.element_size(i);
        vector.row_count = row_count;
        column_builders_[i].Append(vector);
    }

    row_count_ += row_count;
}

// 将 segment 落盘，文件格式与 SegmentReader::Open 对应：
//   [SegmentMeta 序列化区][列0数据][列1数据]...
// path 为目标文件完整路径，文件名（stem）即 segment id。
// 各列数据区的写入委托给 ColumnBuilder::Flush，本方法只负责元数据与整体布局。
bool SegmentBuilder::Flush(const std::filesystem::path& path) {
    // segment id 从文件名解析（命名格式固定：{segment_id}）
    uint32_t segment_id = 0;
    try {
        segment_id = static_cast<uint32_t>(std::stoul(path.filename().string()));
    } catch (const std::exception&) {
        return false;
    }

    // 1. 组装元数据（先以 data_offset=0 序列化一次，确定元数据区大小）
    SegmentMeta meta;
    meta.segment_id = segment_id;
    meta.row_count = row_count_;
    meta.path = path.filename(); // 只存文件名，避免绝对路径耦合
    meta.col_chunk_metas_.reserve(column_builders_.size());
    for (const auto& builder : column_builders_) {
        meta.col_chunk_metas_.push_back(builder.GetMeta());
    }

    BinaryWriter probe;
    meta.Serialize(probe);
    const uint64_t meta_size = probe.GetBuffer().size();

    // 2. 按列顺序计算各列数据在文件中的偏移，并回填元数据
    uint64_t offset = meta_size;
    for (size_t i = 0; i < column_builders_.size(); ++i) {
        meta.col_chunk_metas_[i].data_offset = offset;
        offset += static_cast<uint64_t>(column_builders_[i].row_count()) * column_builders_[i].element_size();
    }

    // 回填后重新序列化（data_offset 为定长字段，总大小与 probe 一致）
    BinaryWriter meta_writer;
    meta.Serialize(meta_writer);
    const auto& meta_buf = meta_writer.GetBuffer();

    // 3. 写文件：元数据区 + 各列数据区（数据区交给 ColumnBuilder 自己写）
    FileWriter file;
    if (!file.Open(path)) {
        return false;
    }

    if (!file.Write(meta_buf.data(), meta_buf.size())) {
        return false;
    }

    // 校验实际写入位置与预计算的布局一致（即元数据区大小未变）
    if (file.Tell() != meta_size) {
        return false;
    }

    for (auto& builder : column_builders_) {
        if (!builder.Flush(file)) {
            return false;
        }
    }

    return file.Close();
}

} // namespace simple_olap