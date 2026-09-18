#pragma once

#include <cstdint>
#include <vector>

#include "../../type.h"
#include "../datachunk.h"
#include "../datastructs.h"

namespace simple_olap {

class VectorBatch;

// ==========================================
// 键编码
// ==========================================
// 编码后的键：逐列 [DataType 标签][字节长度][原始字节] 顺序拼接。
// 类型与长度前缀用于避免跨列拼接碰撞，例如 ("ab","c") 与 ("a","bc")。
using EncodedKey = std::vector<uint8_t>;

// EncodedKey 的哈希器（std::vector<uint8_t> 没有标准哈希特化）
struct EncodedKeyHash {
    size_t operator()(const EncodedKey& key) const noexcept;
};

// 索引条目：键指向的物理位置 + 可见性。
// visible == false 表示数据仍在内存 active/sealed segment：
//   - 唯一性检查必须能看到它（下一次 INSERT 需要发现重复）
//   - 查询查找不能返回它（SELECT 只应看到已落盘数据）
struct IndexEntry {
    RowLocation location;
    bool visible = false;
};

// 键编码器：把一行数据的键列按真实 DataType 精确编码为可哈希字节串。
// 不走 double：INT64 超过 2^53 后转 double 会丢失精度，会导致两个不同的键
// 被误判为同一个。
class KeyEncoder {
  public:
    // DataChunk 路径：列按 schema 顺序布局，row 为 chunk 内的物理行号
    static EncodedKey Encode(const TableSchema& schema, const KeySchema& key, const DataChunk& chunk, uint32_t row);

    // VectorBatch 路径：batch_columns 与 batch.columns 一一对应，
    // 且 batch 不带 selection（row 即物理行号）。
    static EncodedKey Encode(const TableSchema& schema, const KeySchema& key,
                             const std::vector<ColumnId>& batch_columns, const VectorBatch& batch, uint32_t row);

    // 查找路径：字面量按目标列类型转换后编码（IndexScan 的 lookup key）
    static EncodedKey Encode(const TableSchema& schema, const KeySchema& key,
                             const std::vector<ScalarValue>& values);
};

} // namespace simple_olap
