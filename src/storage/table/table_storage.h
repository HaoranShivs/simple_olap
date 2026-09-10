#pragma once

#include <atomic>
#include <memory>
#include <mutex>
#include <unordered_map>

#include "../../execution/vector/vector.h"
#include "../../type.h"
#include "../datachunk.h"
#include "../datastructs.h"
#include "../scan_request.h"
#include "../segment/segment.h"
#include "table_storage_meta.h"

namespace simple_olap {
struct ScanCursor;

class SegmentSource {
  public:
    SegmentSource() = default;

    explicit SegmentSource(std::vector<SegmentId> segments) : segments_(std::move(segments)) {}

    std::optional<SegmentId> Next() {
        auto index = next_.fetch_add(1, std::memory_order_relaxed);

        if (index >= segments_.size()) {
            return std::nullopt;
        }

        return segments_[index];
    }

    // 重置分配进度：每次查询开始前调用。
    // ScanCursor 的初始 segment_id 是 uint32 最大值（魔法值），
    // 首次 Scan 时会从本分配器领取起始 segment；
    // 不重置的话，下一次查询会从上一次的进度继续（或直接耗尽）。
    void Reset() {
        next_.store(0, std::memory_order_relaxed);
    }

  private:
    std::vector<SegmentId> segments_;
    std::atomic<size_t> next_{0};
};

// 单表物理存储对象：对应 DuckDB 的 DataTable
// 职责：
//   - 一张表的 segment 组成（active / sealed / 已落盘）
//   - Append / Scan / Flush
//   - table.meta（TableStorageMeta）的持久化
// 不持有逻辑元数据（name / schema 的权威来源在 Catalog），
// 只持有 schema 的只读引用用于 segment 编解码。
class TableStorage {
  public:
    TableStorage(const TableStorage&) = delete;
    TableStorage& operator=(const TableStorage&) = delete;

    // 创建新表：建目录 tables/{table_id} 并写入 table.meta
    static std::unique_ptr<TableStorage> Create(TableId table_id, const TableSchema& schema,
                                                const std::filesystem::path& tables_root);

    // 从硬盘打开已有表（读取 table.meta）。
    // schema 的权威来源在 Catalog，由上层 StorageManager 取出后传入。
    static std::unique_ptr<TableStorage> Open(TableId table_id, const TableSchema& schema,
                                              const std::filesystem::path& tables_root);

    ~TableStorage();

    // ---------- 写 ----------

    void Append(const DataChunk& input);

    // 将内存中待刷盘的 segment 落盘，并持久化 table.meta
    bool Flush();

    // ---------- 读 ----------

    // 跨 segment 扫描：由 cursor 记录推进位置，输出一个 VectorBatch
    bool Scan(const ScanOptions& options, ScanCursor& cursor, VectorBatch& output);

    // ---------- 观察接口 ----------

    // 重置 segment 分配器：每次查询开始前由执行引擎调用。
    // ScanCursor 初始 segment_id 为 uint32 最大值（魔法值），
    // 首次 Scan 时从分配器领取起始 segment；并行扫描时多个 worker
    // 通过同一个原子分配器领取互不重叠的 segment。
    void ResetSegmentAllocator() {
        segmentallocator_.Reset();
    }

    TableId id() const noexcept {
        return metadata_.table_id;
    }

    // 活跃 segment 当前已积累的行数（未封存、scan 不可见）
    uint32_t active_segment_row_count() const noexcept {
        return active_segment_ ? active_segment_->row_count() : 0;
    }

    // segment 总数（已落盘 + 内存中待刷盘）
    size_t segment_count() const noexcept {
        return metadata_.segment_ids.size() + sealed_segments_.size();
    }

    // 表数据目录
    const std::filesystem::path& path() const noexcept {
        return table_path_;
    }

  private:
    TableStorage(TableId table_id, std::filesystem::path table_path, const TableSchema& schema,
                 TableStorageMeta metadata);

    void SealActiveSegment();

    void CreateActiveSegment();

    bool SaveMeta() const;

    // 获取（必要时打开）指定 id 的 SegmentReader；失败返回 nullptr
    SegmentReader* GetSegmentReader(SegmentId id);

    // 从共享 segment 分配器领取下一个 segment 并重置游标的 segment 内状态；
    // 分配器耗尽返回 false。
    // 并行扫描时多个 worker 通过同一个原子分配器领取互不重叠的 segment。
    bool AdvanceCursorToNextSegment(ScanCursor& cursor);

    TableId table_id_;

    // schema 的权威来源在 Catalog；segment 编解码只需要只读访问
    const TableSchema* schema_ = nullptr;

    TableStorageMeta metadata_;

    std::filesystem::path table_path_;

    SegmentSource segmentallocator_;

    // 内存中待刷盘的 segment：id -> 填满的 SegmentBuilder
    std::unordered_map<SegmentId, std::unique_ptr<SegmentBuilder>> sealed_segments_;

    // 已落盘 segment 的只读视图缓存：id -> SegmentReader（懒加载，避免重复 mmap）。
    // 并行扫描时多个 worker 线程会并发触发懒加载，用互斥锁保护。
    std::mutex reader_cache_mutex_;
    std::unordered_map<SegmentId, std::unique_ptr<SegmentReader>> reader_cache_;

    // append state
    SegmentId active_segment_id_ = 0;

    SegmentId next_segment_id_ = 0;

    std::unique_ptr<SegmentBuilder> active_segment_;
};

} // namespace simple_olap
