#pragma once

#include <cstdint>
#include <vector>
#include <cstring>
#include <stdexcept>
#include "../../type.h"

namespace simple_olap
{
    /// @brief 向量化数据载体
    /// 每次承载一批数据（默认1024行），是算子间传递数据的统一格式。
    /// 【伏笔】：buffer 使用连续内存，第二阶段可直接用 AVX2 _mm256_load 加载。
    class VectorBatch
    {
    public:
        static constexpr uint32_t BATCH_SIZE = 1024;

        /// @brief 单列数据，使用 uint8_t buffer 做通用字节存储
        struct ColumnData
        {
            DataType type = DataType::INVALID;
            std::vector<uint8_t> buffer; // 原始连续字节存储
            uint32_t count = 0;            // 当前有效元素数量
            std::vector<int> bit_map;

            /// 类型化只读指针 —— 第二阶段 SIMD 读取的入口
            template <typename T>
            const T *data() const
            {
                return reinterpret_cast<const T *>(buffer.data());
            }

            /// 类型化可写指针
            template <typename T>
            T *mutable_data()
            {
                return reinterpret_cast<T *>(buffer.data());
            }

            /// 按元素数量重新分配 buffer
            void Resize(uint32_t new_count)
            {
                count = new_count;
                uint32_t elem_size = TypeElemSize(type);
                buffer.resize(count * elem_size);
            }

            /// 将新数据追加到 buffer 末尾（用于 Scan 算子批量拷贝）
            void CopyFrom(const void *src, uint32_t elem_count)
            {
                uint32_t elem_size = TypeElemSize(type);
                buffer.resize(elem_count * elem_size);
                std::memcpy(buffer.data(), src, elem_count * elem_size);
                count = elem_count;
            }

        private:
            static uint32_t TypeElemSize(DataType t)
            {
                switch (t)
                {
                case DataType::INT32:
                    return sizeof(int32_t);
                case DataType::INT64:
                    return sizeof(int64_t);
                case DataType::FLOAT:
                    return sizeof(float);
                default:
                    throw std::runtime_error("Unsupported type in ColumnData");
                }
            }
        };

        // ============ 公共成员 ============

        std::vector<ColumnData> columns;  ///< 各列数据
        std::vector<uint32_t> sel_vector; ///< 选择向量：有效行的局部索引
        uint32_t size = 0;                  ///< 当前 batch 有效行数

        /// 添加一列
        void AddColumn(DataType type)
        {
            ColumnData col;
            col.type = type;
            columns.push_back(std::move(col));
        }

        /// 重置 batch 状态（不释放内存，减少分配）
        void Reset()
        {
            for (auto &col : columns)
            {
                col.count = 0;
                col.buffer.clear();
            }
            sel_vector.clear();
            size = 0;
        }

        uint32_t ColumnCount() const { return columns.size(); }
    };
} // namespace simple
