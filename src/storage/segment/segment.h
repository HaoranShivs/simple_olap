#pragma once

#include "../datachunk.h"
#include "../datastructs.h"
#include "../file/mappedfile.h"
#include "columnchunk.h"
#include <memory>
#include <unordered_map>

namespace simple_olap {
// SegmentId / ScanOptions 定义于 ../datastructs.h
class VectorBatch;

enum class SegmentMatch : uint8_t { SKIP, ALL_MATCH, NEED_FILTER };

struct SegmentFilterDecision {
    bool skip_segment = false;

    // 与 ScanOptions::predicates
    // 一一对应。
    //
    // 1:
    //   该 predicate 这个 segment
    //   需要逐行精确执行。
    //
    // 0:
    //   metadata 已证明全部满足。
    std::vector<uint8_t> row_filter_mask;
};

class SegmentReader {
  public:
    // 从硬盘引入Segment：mmap 映射 segment 文件，解析元数据并建立列块视图
    // 失败返回 nullptr；成功返回的对象由调用方负责释放
    static SegmentReader* Open(SegmentId id, const std::filesystem::path& root_dir);

    ~SegmentReader();

    //--------------读取元数据--------------------

    uint64_t id() const;

    uint32_t row_count() const;

    const ColumnChunkMeta& GetColumnMeta(ColumnId id) const noexcept;

    // 用 segment 列统计信息（min/max）对每个 pushed predicate 做三态判断：
    //   SKIP        -> 整个 segment 不可能有满足条件的行
    //   ALL_MATCH   -> 该 predicate 对整个 segment 恒成立，行级无需再判断
    //   NEED_FILTER -> 该 predicate 需要进入行级过滤
    // 返回的 SegmentFilterDecision::row_filter_mask 与 predicates 一一对应。
    SegmentFilterDecision EvaluatePredicates(const std::vector<Condition>& predicates) const;

    // 以 offset 为 segment 内部的起点，扫描最多 1024 行所要求列的数据到 output。
    // 本方法不再负责 predicate 语义（metadata 判断 / 行过滤由 StorageManager 编排）。
    bool GetVectorBatch(const ScanOptions& scanoptions, uint32_t offset, VectorBatch& output);

  private:
    SegmentReader(uint64_t segment_id, SegmentMeta matedata, MappedFile file);

    ColumnChunkReader OpenColumn(uint32_t column_id) const;

    uint64_t segment_id_;

    SegmentMeta metadata_;

    // 整个 segment 文件的只读内存映射；
    // ColumnChunkReader 的 data_ 指针直接指向映射区域，生命周期由本对象保证
    MappedFile mapped_file_;

    // 列块缓存：column_id -> ColumnChunkReader 视图
    std::unordered_map<ColumnChunkId, ColumnChunkReader> columns_;
};

class SegmentBuilder {
  public:
    explicit SegmentBuilder(const TableSchema& schema);

    void Append(const DataChunk& batch);

    bool Flush(const std::filesystem::path& path);

    uint32_t row_count() const {
        return row_count_;
    }

    // // 将自身所有列转换为 ColumnChunkReader 列表
    // std::vector<ColumnChunkReader> ToColumnChunks() const;

  private:
    uint32_t row_count_;

    std::vector<ColumnBuilder> column_builders_;
};

} // namespace simple_olap
