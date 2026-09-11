#include "table_storage.h"

#include <algorithm>
#include <fstream>
#include <stdexcept>
#include <system_error>

#include "../../execution/vector/vector.h"
#include "../scan/parallel_scan_session.h"
#include "../scan/row_filter.h"

namespace simple_olap {

// ---------- 创建 / 打开 ----------

std::unique_ptr<TableStorage> TableStorage::Create(TableId table_id, const TableSchema& schema,
                                                   const std::filesystem::path& tables_root, BufferPool* buffer_pool) {
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
    return std::unique_ptr<TableStorage>(
        new TableStorage(table_id, table_path, schema, std::move(metadata), buffer_pool));
}

std::unique_ptr<TableStorage> TableStorage::Open(TableId table_id, const TableSchema& schema,
                                                 const std::filesystem::path& tables_root, BufferPool* buffer_pool) {
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
    return std::unique_ptr<TableStorage>(
        new TableStorage(table_id, table_path, schema, std::move(metadata), buffer_pool));
}

TableStorage::TableStorage(TableId table_id, std::filesystem::path table_path, const TableSchema& schema,
                           TableStorageMeta metadata, BufferPool* buffer_pool)
    : table_id_(table_id), schema_(&schema), metadata_(std::move(metadata)), table_path_(std::move(table_path)),
      buffer_pool_(buffer_pool) {
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

std::shared_ptr<const PreparedScanPredicates> TableStorage::PrepareScanPredicates(const ScanOptions& options) const {
    // schema_ 的权威来源在 Catalog，生命周期覆盖本表。
    return std::make_shared<const PreparedScanPredicates>(PreparedScanPredicates::Build(options, *schema_));
}

bool TableStorage::AdvanceCursorToNextSegment(ScanCursor& cursor) {
    // segment_id 是 metadata_.segment_ids 的下标，进度保存在游标自身。
    // 首次调用把哨兵值提升为 0，之后逐 1 前进，耗尽返回 false。
    if (cursor.segment_id == kInvalidSegmentId) {
        cursor.segment_id = 0;
    } else {
        cursor.segment_id += 1;
    }

    if (cursor.segment_id >= metadata_.segment_ids.size()) {
        return false; // 所有 segment 都已扫描完
    }

    cursor.offset_in_segment = 0;
    cursor.segment_decision_valid = false;
    cursor.row_filter_mask.clear();
    return true;
}

SegmentReader* TableStorage::GetSegmentReader(SegmentId id) {
    // 懒加载：首次访问时 mmap 打开并缓存，后续复用。
    // 并行扫描时多个 worker 线程会并发调用，缓存读写必须在锁内完成。
    std::lock_guard<std::mutex> lock(reader_cache_mutex_);

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
bool TableStorage::ScanSegment(SegmentId segment_id, const ScanOptions& options, SegmentScanCursor& cursor,
                               VectorBatch& output) {
    SegmentReader* reader = GetSegmentReader(segment_id);
    if (reader == nullptr) {
        // 打开失败：本 segment 不可读，视为读完
        return false;
    }

    // =====================================
    // 第一层：每个 segment 只做一次 metadata 判断
    // =====================================
    if (!cursor.decision_valid) {
        const auto decision = reader->EvaluatePredicates(options.predicates);

        if (decision.skip_segment) {
            // 整个 segment 都不可能有满足条件的行
            return false;
        }

        cursor.row_filter_mask = decision.row_filter_mask;
        cursor.decision_valid = true;
    }

    // =====================================
    // 第二层：读取 physical batch
    // =====================================
    const bool scanned = reader->GetVectorBatch(options, cursor.offset, output);

    if (!scanned || output.size == 0) {
        // 本 segment 已读完
        return false;
    }

    // 非常重要：
    // cursor 必须按“扫描的物理行数”前进，而不是过滤后的有效行数。
    const uint32_t physical_rows = output.size;
    cursor.offset += physical_rows;

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
        if (cursor.prepared_predicates == nullptr) {
            // 调用方（Scan / ParallelScanSession）必须提前准备谓词计划
            throw std::runtime_error("scan predicates not prepared before ScanSegment");
        }

        // SIMD/scalar 类型化比较 -> SelectionMask AND -> sel_vector
        StorageRowFilter::Apply(*cursor.prepared_predicates, cursor.row_filter_mask, output);

        if (output.size == 0) {
            // 本批全部被过滤：调用方继续读下一批
            return true;
        }
    } else {
        // 所有 pushed predicate 对该 segment 都 ALL_MATCH
        output.sel_vector.clear();
    }

    return true;
}

bool TableStorage::Scan(const ScanOptions& options, ScanCursor& cursor, VectorBatch& output) {
    const auto& segment_ids = metadata_.segment_ids;

    // 惰性准备：第一次 Scan 时 Build 一次，之后所有 batch / segment 复用。
    if (cursor.prepared_predicates == nullptr && !options.predicates.empty()) {
        cursor.prepared_predicates = PrepareScanPredicates(options);
    }

    while (1) {
        if (cursor.segment_id == kInvalidSegmentId) {
            if (!AdvanceCursorToNextSegment(cursor)) {
                return false; // 没有 segment
            }
        }
        const SegmentId seg_id = segment_ids[cursor.segment_id];

        // 单 segment 扫描：metadata pruning / batch 读取 / row filtering
        // 与并行路径共用同一份逻辑（ScanSegment）
        SegmentScanCursor segment_cursor;
        segment_cursor.offset = cursor.offset_in_segment;
        segment_cursor.decision_valid = cursor.segment_decision_valid;
        segment_cursor.row_filter_mask = cursor.row_filter_mask;
        segment_cursor.prepared_predicates = cursor.prepared_predicates.get();

        const bool got = ScanSegment(seg_id, options, segment_cursor, output);

        // 把 segment 内游标状态同步回跨 segment 游标
        cursor.offset_in_segment = segment_cursor.offset;
        cursor.segment_decision_valid = segment_cursor.decision_valid;
        cursor.row_filter_mask = std::move(segment_cursor.row_filter_mask);

        if (!got) {
            // 本 segment 读完（或被 pruning 跳过 / 打开失败）：推进到下一个
            if (!AdvanceCursorToNextSegment(cursor)) {
                return false; // 代表没segment了
            }
            continue;
        }

        if (output.size == 0) {
            // 本批全部被行过滤：继续读下一批
            continue;
        }

        return true;
    }

    // 所有 segment 都已读完
    output.Reset();
    return false;
}

// ---------- 并行扫描 ----------

std::shared_ptr<BatchStream> TableStorage::CreateParallelScan(const ScanOptions& options, size_t scan_threads,
                                                              size_t queue_capacity) {
    // 谓词计划只准备一次，所有 scan worker 只读共享。
    std::shared_ptr<const PreparedScanPredicates> prepared;
    if (!options.predicates.empty()) {
        prepared = PrepareScanPredicates(options);
    }

    return std::make_shared<ParallelScanSession>(this, metadata_.segment_ids, options, std::move(prepared),
                                                 scan_threads, queue_capacity, buffer_pool_);
}

} // namespace simple_olap
