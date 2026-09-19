#pragma once

#include <cstdint>
#include <cstring>
#include <string>
#include <type_traits>

#include "../execution/batch_utils.h"
#include "../execution/expression/exec_expression.h"
#include "../execution/vector/vector.h"
#include "query_result.h"

namespace simple_olap {

// ============================================================
// ResultDigest：与行序无关的结果摘要，用于 A/B 正确性校验
// ============================================================
//
// 只比较 row count 无法区分 SUM=123 与 SUM=456 这类错误；
// 对每个输出行计算 64-bit hash，然后累加：
//
//   hash1 += H(row)
//   hash2 += H(row) * H(row)
//
// 累加与行序无关，因此多线程并行输出（顺序不做保证）也能与单线程结果比较。
struct ResultDigest {
    uint64_t row_count = 0;
    uint64_t hash1 = 0;
    uint64_t hash2 = 0;
};

inline bool SameDigest(const ResultDigest& lhs, const ResultDigest& rhs) {
    return lhs.row_count == rhs.row_count && lhs.hash1 == rhs.hash1 && lhs.hash2 == rhs.hash2;
}

// splitmix64 finalizer：把任意 64-bit 输入打散。
inline uint64_t MixHash(uint64_t x) {
    x += 0x9e3779b97f4a7c15ULL;
    x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ULL;
    x = (x ^ (x >> 27)) * 0x94d049bb133111ebULL;
    return x ^ (x >> 31);
}

inline uint64_t HashValueBits(const ExecValue& value) {
    return std::visit(
        [](const auto& raw) -> uint64_t {
            using T = std::decay_t<decltype(raw)>;
            if constexpr (std::is_same_v<T, std::string>) {
                uint64_t h = 1469598103934665603ULL; // FNV-1a offset basis
                for (unsigned char c : raw) {
                    h ^= c;
                    h *= 1099511628211ULL;
                }
                return MixHash(h);
            } else if constexpr (std::is_same_v<T, float>) {
                uint32_t bits = 0;
                std::memcpy(&bits, &raw, sizeof(bits));
                return MixHash(bits);
            } else if constexpr (std::is_same_v<T, double>) {
                uint64_t bits = 0;
                std::memcpy(&bits, &raw, sizeof(bits));
                return MixHash(bits);
            } else if constexpr (std::is_same_v<T, bool>) {
                return MixHash(raw ? 1ULL : 0ULL);
            } else {
                return MixHash(static_cast<uint64_t>(static_cast<int64_t>(raw)));
            }
        },
        value);
}

inline uint64_t HashRow(const VectorBatch& batch, uint32_t physical_row) {
    uint64_t row_hash = 0x243f6a8885a308d3ULL;
    for (const ColumnData& column : batch.columns) {
        row_hash = MixHash(row_hash ^ HashValueBits(ReadExecValue(column, physical_row)));
    }
    return row_hash;
}

inline void AccumulateRow(ResultDigest& digest, const VectorBatch& batch, uint32_t physical_row) {
    const uint64_t row_hash = HashRow(batch, physical_row);
    ++digest.row_count;
    digest.hash1 += row_hash;
    digest.hash2 += row_hash * row_hash;
}

inline void AccumulateBatch(ResultDigest& digest, const VectorBatch& batch) {
    ForEachActiveRow(batch, [&](uint32_t physical_row) { AccumulateRow(digest, batch, physical_row); });
}

inline ResultDigest ComputeResultDigest(const VectorBatch& batch) {
    ResultDigest digest;
    AccumulateBatch(digest, batch);
    return digest;
}

inline ResultDigest ComputeResultDigest(const QueryResult& result) {
    ResultDigest digest;
    for (const VectorBatch& batch : result.chunks) {
        AccumulateBatch(digest, batch);
    }
    return digest;
}

} // namespace simple_olap
