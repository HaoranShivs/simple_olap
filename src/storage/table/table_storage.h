#pragma once

#include <atomic>
#include <memory>
#include <mutex>
#include <unordered_map>

#include "../../execution/vector/vector.h"
#include "../../memory/buffer_pool/buffer_pool.h"
#include "../../type.h"
#include "../datachunk.h"
#include "../datastructs.h"
#include "../scan/batch_stream.h"
#include "../scan_request.h"
#include "../segment/segment.h"
#include "table_storage_meta.h"

namespace simple_olap {
// 前置声明：扫描游标（定义见 ../datastructs.h）
struct ScanCursor;
struct SegmentScanCursor;

// 单表物理存储对象
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
                                                const std::filesystem::path& tables_root,
                                                BufferPool* buffer_pool = nullptr);

    // 从硬盘打开已有表（读取 table.meta）。
    // schema 的权威来源在 Catalog，由上层 StorageManager 取出后传入。
    static std::unique_ptr<TableStorage> Open(TableId table_id, const TableSchema& schema,
                                              const std::filesystem::path& tables_root,
                                              BufferPool* buffer_pool = nullptr);

    ~TableStorage();

    // ---------- 写 ----------

    void Append(const DataChunk& input);

    // 将内存中待刷盘的 segment 落盘，并持久化 table.meta
    bool Flush();

    // ---------- 读 ----------

    // 跨 segment 扫描：由 cursor 记录推进位置，输出一个 VectorBatch
    bool Scan(const ScanOptions& options, ScanCursor& cursor, VectorBatch& output);

    // 单 segment 扫描：只读取指定 segment 内 [cursor.offset, ...) 的数据，
    // 绝不推进到其他 segment。metadata pruning / batch 读取 / row filtering
    // 与串行 Scan() 共用完全相同的逻辑。
    // 返回 false 表示本 segment 已读完（或被 metadata pruning 跳过）。
    bool ScanSegment(SegmentId segment_id, const ScanOptions& options, SegmentScanCursor& cursor, VectorBatch& output);

    // 并行扫描入口：创建 ParallelScanSession（BatchStream）。
    // 调用方 Start() 后通过 BatchStream::Next() 拉取批次；
    // scan 线程在 storage 内部由 session 管理（atomic next_segment 分配）。
    // scan worker 产出的 VectorBatch 绑定本表持有的 BufferPool。
    std::shared_ptr<BatchStream> CreateParallelScan(const ScanOptions& options, size_t scan_threads,
                                                    size_t queue_capacity);

    // ---------- 观察接口 ----------

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
                 TableStorageMeta metadata, BufferPool* buffer_pool);

    void SealActiveSegment();

    void CreateActiveSegment();

    bool SaveMeta() const;

    // 获取（必要时打开）指定 id 的 SegmentReader；失败返回 nullptr
    SegmentReader* GetSegmentReader(SegmentId id);

    // 把游标推进到下一个 segment（进度保存在游标内）。
    // 首次调用从 index 0 开始，耗尽返回 false。
    bool AdvanceCursorToNextSegment(ScanCursor& cursor);

    TableId table_id_;

    // schema 的权威来源在 Catalog；segment 编解码只需要只读访问
    const TableSchema* schema_ = nullptr;

    TableStorageMeta metadata_;

    std::filesystem::path table_path_;

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

    // 注入的 BufferPool：scan worker 产出的 VectorBatch 绑定它。
    BufferPool* buffer_pool_ = nullptr;
};

} // namespace simple_olap
