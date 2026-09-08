#include "table_storage.h"

#include <algorithm>
#include <fstream>
#include <stdexcept>
#include <system_error>

#include "../../execution/vector/vector.h"

namespace simple_olap {
namespace {
// 按行读取列值并转为 double，供 where 行级过滤比较；非数值类型返回 false
bool ColumnValueAsDouble(const ColumnData& col, uint32_t row, double& out) {
    switch (col.type) {
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
bool CompareDouble(double lhs, CmpOp op, double rhs) {
    switch (op) {
    case CmpOp::EQ:
        return lhs == rhs;
    case CmpOp::NE:
        return lhs != rhs;
    case CmpOp::GT:
        return lhs > rhs;
    case CmpOp::GE:
        return lhs >= rhs;
    case CmpOp::LT:
        return lhs < rhs;
    case CmpOp::LE:
        return lhs <= rhs;
    }
    return false;
}

// 从 Condition 的 variant 值中提取 double（仅数值；字符串返回 false）
bool ColumnValueAsDoubleFromVariant(const std::variant<int32_t, int64_t, double, std::string>& value, double& out) {
    if (std::holds_alternative<int32_t>(value)) {
        out = static_cast<double>(std::get<int32_t>(value));
        return true;
    }
    if (std::holds_alternative<int64_t>(value)) {
        out = static_cast<double>(std::get<int64_t>(value));
        return true;
    }
    if (std::holds_alternative<double>(value)) {
        out = std::get<double>(value);
        return true;
    }
    return false;
}

// 行级精确判断单个 predicate：lhs op rhs。
// 一个 predicate 一旦进入 Storage，就意味着 Optimizer 已承诺 Storage 可精确执行。
bool EvaluateCondition(const ColumnData& column, uint32_t row, const Condition& cond) {
    double lhs = 0.0;
    double rhs = 0.0;

    if (!ColumnValueAsDouble(column, row, lhs)) {
        throw std::runtime_error("pushed predicate contains unsupported column type");
    }

    if (!ColumnValueAsDoubleFromVariant(cond.value, rhs)) {
        throw std::runtime_error("pushed predicate contains unsupported literal type");
    }

    return CompareDouble(lhs, cond.op, rhs);
}

// 多 predicate 行过滤：按 predicate 逐个收缩 selection vector。
// 好处：predicate A 把 1024 行过滤到 50 行后，predicate B 只需执行 50 次。
void ApplyRowPredicates(const ScanOptions& options, const std::vector<uint8_t>& row_filter_mask, VectorBatch& output) {
    const uint32_t physical_rows = output.size;

    std::vector<uint32_t> selection;
    selection.reserve(physical_rows);

    for (uint32_t row = 0; row < physical_rows; ++row) {
        selection.push_back(row);
    }

    for (size_t p = 0; p < options.predicates.size(); ++p) {
        // metadata 已经证明这个 predicate 对整个 segment ALL_MATCH，跳过
        if (!row_filter_mask[p]) {
            continue;
        }

        const Condition& cond = options.predicates[p];

        // 定位 predicate 列在 output 中的下标
        const auto it = std::find(options.columns.begin(), options.columns.end(), cond.column);
        if (it == options.columns.end()) {
            throw std::runtime_error("predicate column is not scanned");
        }

        const size_t slot = static_cast<size_t>(it - options.columns.begin());
        const ColumnData& column = output.columns[slot];

        size_t write = 0;
        for (size_t read = 0; read < selection.size(); ++read) {
            const uint32_t row = selection[read];
            if (EvaluateCondition(column, row, cond)) {
                selection[write++] = row;
            }
        }
        selection.resize(write);

        if (selection.empty()) {
            break;
        }
    }

    if (selection.size() == physical_rows) {
        // identity selection：全部行通过，无需 sel_vector
        output.sel_vector.clear();
        output.size = physical_rows;
        return;
    }

    output.sel_vector = std::move(selection);
    output.size = static_cast<uint32_t>(output.sel_vector.size());
}
} // namespace

// ---------- 创建 / 打开 ----------

std::unique_ptr<TableStorage> TableStorage::Create(TableId table_id, const TableSchema& schema,
                                                   const std::filesystem::path& tables_root) {
    // 1. 创建表数据目录：tables_root / {table_id}
    const std::filesystem::path table_path = tables_root / std::to_string(table_id);
    std::error_code ec;
    std::filesystem::create_directories(table_path, ec);
    if (ec) {
        return nullptr;
    }

    // 2. 组装表存储元数据并写入 table_path / table.meta
    //    （schema 的权威来源在 Catalog，table.meta 只记录物理布局）
    TableStorageMeta metadata;
    metadata.table_id = table_id;

    BinaryWriter writer;
    metadata.Serialize(writer);

    const std::filesystem::path meta_path = table_path / "table.meta";
    std::ofstream file(meta_path, std::ios::binary | std::ios::trunc);
    if (!file) {
        return nullptr;
    }

    const auto& buffer = writer.GetBuffer();
    file.write(reinterpret_cast<const char*>(buffer.data()), static_cast<std::streamsize>(buffer.size()));
    if (!file.good()) {
        return nullptr;
    }

    // 3. 构造 TableStorage（构造时立即创建空 SegmentBuilder）
    return std::unique_ptr<TableStorage>(new TableStorage(table_id, table_path, schema, std::move(metadata)));
}

std::unique_ptr<TableStorage> TableStorage::Open(TableId table_id, const TableSchema& schema,
                                                 const std::filesystem::path& tables_root) {
    // 1. 读取并反序列化 table.meta（segment 布局）
    const std::filesystem::path table_path = tables_root / std::to_string(table_id);
    const std::filesystem::path meta_path = table_path / "table.meta";

    std::ifstream in(meta_path, std::ios::binary);
    if (!in) {
        return nullptr;
    }
    std::vector<uint8_t> buffer((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    in.close();

    BinaryReader reader(buffer);
    TableStorageMeta metadata = TableStorageMeta::Deserialize(reader);

    // 2. 校验 id 一致性
    if (metadata.table_id != table_id) {
        return nullptr;
    }

    // 3. 构造 TableStorage（schema 由上层 StorageManager 从 Catalog 取出后传入）
    return std::unique_ptr<TableStorage>(new TableStorage(table_id, table_path, schema, std::move(metadata)));
}

TableStorage::TableStorage(TableId table_id, std::filesystem::path table_path, const TableSchema& schema,
                           TableStorageMeta metadata)
    : table_id_(table_id), schema_(&schema), metadata_(std::move(metadata)), table_path_(std::move(table_path)) {
    // 新 segment 的 id 从已落盘最大 id + 1 开始
    SegmentId next = 0;
    for (SegmentId id : metadata_.segment_ids) {
        next = std::max(next, id + 1);
    }
    next_segment_id_ = next;
    CreateActiveSegment();
}

TableStorage::~TableStorage() {
    // 析构兜底：把非空的活跃 segment 封存并落盘，防止进程退出丢数据。
    // 注意：这里只负责数据落盘，table.meta 的持久化由 Flush() 负责。
    if (active_segment_ != nullptr && active_segment_->row_count() > 0) {
        SealActiveSegment();
        CreateActiveSegment();
    }
    if (!sealed_segments_.empty()) {
        Flush();
    }
}

// ---------- 写路径 ----------

void TableStorage::Append(const DataChunk& input) {
    // 浅拷贝，共享底层缓冲；后续用 Slice 切块
    DataChunk chunk = input;
    while (chunk.size() > 0) {
        // 活跃 segment 的剩余容量
        uint32_t remaining = kMaxSegmentRowCount - active_segment_->row_count();

        if (chunk.size() <= remaining) {
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

void TableStorage::SealActiveSegment() {
    // 活跃 segment 已填满：不立即写盘，移入待刷盘 map，等待 Flush() 统一落盘
    sealed_segments_[active_segment_id_] = std::move(active_segment_);
}

bool TableStorage::Flush() {
    // 0. 活跃 segment 非空时先封存：Scan 只读已落盘 segment，
    //    不封存的话刚 Append 的数据对查询不可见（析构兜底也是同样语义）
    if (active_segment_ != nullptr && active_segment_->row_count() > 0) {
        SealActiveSegment();
        CreateActiveSegment();
    }

    // 1. 将内存中所有待刷盘 segment 写盘，并把 id 登记进 TableStorageMeta。
    //    文件命名格式固定：table_path_ / {segment_id}，与 SegmentBuilder::Flush /
    //    SegmentReader::Open 的约定一致。
    for (auto& entry : sealed_segments_) {
        const std::filesystem::path seg_path = table_path_ / std::to_string(entry.first);
        if (entry.second->Flush(seg_path)) {
            metadata_.segment_ids.push_back(entry.first);
        }
    }
    sealed_segments_.clear();

    // 2. 持久化 table.meta（segment id 列表是唯一需要同步的物理元数据）
    return SaveMeta();
}

bool TableStorage::SaveMeta() const {
    BinaryWriter writer;
    metadata_.Serialize(writer);

    const std::filesystem::path meta_path = table_path_ / "table.meta";
    std::ofstream file(meta_path, std::ios::binary | std::ios::trunc);
    if (!file) {
        return false;
    }

    const auto& buffer = writer.GetBuffer();
    file.write(reinterpret_cast<const char*>(buffer.data()), static_cast<std::streamsize>(buffer.size()));

    return file.good();
}

void TableStorage::CreateActiveSegment() {
    active_segment_id_ = next_segment_id_++;
    active_segment_ = std::make_unique<SegmentBuilder>(*schema_);
}

// ---------- 读路径 ----------

SegmentReader* TableStorage::GetSegmentReader(SegmentId id) {
    // 懒加载：首次访问时 mmap 打开并缓存，后续复用
    auto it = reader_cache_.find(id);
    if (it != reader_cache_.end()) {
        return it->second.get();
    }

    SegmentReader* reader = SegmentReader::Open(id, table_path_);
    if (reader == nullptr) {
        return nullptr;
    }
    reader_cache_.emplace(id, std::unique_ptr<SegmentReader>(reader));
    return reader_cache_[id].get();
}

// 注意:
//   Optimizer 只下推 Storage 能精确执行的谓词；执行失败应报内部错误，
//   而不是保守保留导致 SQL 结果错误。
//
//   避免重复过滤是 Optimizer 的职责。
//
//    row_filter_mask 为 0 的 predicate 在本 segment 内不再逐行判断。
bool TableStorage::Scan(const ScanOptions& options, ScanCursor& cursor, VectorBatch& output) {
    const auto& segment_ids = metadata_.segment_ids;

    while (cursor.segment_id < segment_ids.size()) {
        const SegmentId seg_id = segment_ids[cursor.segment_id];

        SegmentReader* reader = GetSegmentReader(seg_id);
        if (reader == nullptr) {
            // 打开失败：跳过该 segment，继续尝试下一个
            cursor.AdvanceSegment();
            continue;
        }

        // =====================================
        // 第一层：每个 segment 只做一次 metadata 判断
        // =====================================
        if (!cursor.segment_decision_valid) {
            const auto decision = reader->EvaluatePredicates(options.predicates);

            if (decision.skip_segment) {
                // 整个 segment 都不可能有满足条件的行，直接跳过
                cursor.AdvanceSegment();
                continue;
            }

            cursor.row_filter_mask = decision.row_filter_mask;
            cursor.segment_decision_valid = true;
        }

        // =====================================
        // 第二层：读取 physical batch
        // =====================================
        const bool scanned = reader->GetVectorBatch(options, cursor.offset_in_segment, output);

        if (!scanned || output.size == 0) {
            // 本 segment 已读完，推进到下一个
            cursor.AdvanceSegment();
            continue;
        }

        // 非常重要：
        // cursor 必须按“扫描的物理行数”前进，而不是过滤后的有效行数。
        const uint32_t scanned_rows = output.size;
        cursor.offset_in_segment += scanned_rows;

        // =====================================
        // 第三层：只有 metadata 无法决定的 predicate 才 row filter
        // =====================================
        bool need_row_filter = false;
        for (uint8_t flag : cursor.row_filter_mask) {
            if (flag) {
                need_row_filter = true;
                break;
            }
        }

        if (need_row_filter) {
            ApplyRowPredicates(options, cursor.row_filter_mask, output);

            if (output.size == 0) {
                // 本批全部被过滤：继续读下一批
                continue;
            }
        } else {
            // 所有 pushed predicate 对该 segment 都 ALL_MATCH
            output.sel_vector.clear();
        }

        return true;
    }

    // 所有 segment 都已读完
    output.Reset();
    return false;
}

} // namespace simple_olap
