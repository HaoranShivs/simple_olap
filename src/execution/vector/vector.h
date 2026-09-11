#pragma once

#include "../../memory/buffer_pool/buffer_pool.h"
#include "../../type.h"
#include <cstdint>
#include <utility>
#include <vector>

namespace simple_olap {

// ---------- ColumnData ----------

// 单列连续字节缓冲，支持两种模式：
//   - 视图（is_view() == true）：buffer 指向外部内存（如 mmap 映射区），零拷贝、不持有所有权，
//     外部数据生命周期由提供方保证。
//   - 实际内存（is_view() == false）：buffer 指向 BufferPool 分配的 owned_buffer_，
//     数据由本对象持有（BufferHandle 析构 / Reset 时归还 BufferPool）。
// 视图需要就地修改（如按选择向量压缩）时，先用 Materialize() 转为持有。
struct ColumnData {
    ColumnData() = default;

    explicit ColumnData(DataType type, BufferPool* buffer_pool) : type(type), buffer_pool_(buffer_pool) {}

    DataType type = DataType::INVALID;
    uint8_t* buffer = nullptr; // 原始连续字节存储（owned 或 borrowed）
    uint32_t count = 0;        // 当前元素数量

    // 是否处于视图模式
    bool is_view() const;

    // 类型化只读指针，供 SIMD 读取使用
    template <typename T> const T* data() const {
        return reinterpret_cast<const T*>(buffer);
    }

    // 类型化可写指针
    template <typename T> T* mutable_data() {
        return reinterpret_cast<T*>(buffer);
    }

    // 按元素数量重新分配 buffer（转为实际内存模式；count 为有效元素数量）。
    // 当前 Handle 容量足够时直接复用，不 Acquire / 不 Release。
    void Resize(uint32_t new_count);

    // 装载数据：
    //   is_view == true ：零拷贝，buffer 直接指向 src（src 必须比本对象长寿）
    //   is_view == false：深拷贝 src 到 BufferPool 缓冲
    void CopyFrom(const void* src, uint32_t elem_count, bool is_view = true);

    // 视图 -> 实际内存：把当前指向的数据深拷贝进 BufferPool 缓冲。
    // 已是实际内存模式时为 no-op。用于在修改数据前解除对外部内存的依赖。
    void Materialize();

    // 用给定的 BufferHandle 整体替换本列数据（实际内存模式）
    void ReplaceWith(BufferHandle&& buffer_handle, uint32_t new_count);

    // 清空状态：owned_buffer_ 归还 BufferPool（≠ free）；视图模式只断开指针。
    void Reset();

  private:
    bool is_view_ = true; // 视图模式：buffer 指向外部内存

    BufferPool* buffer_pool_ = nullptr;

    // 实际内存模式：buffer 指向这里（BufferPool 分配）。
    BufferHandle owned_buffer_;
};

// ---------- VectorBatch ----------

// 向量化数据载体：算子间传递数据的统一格式，每次承载一批（默认 1024 行）。
// buffer 使用连续内存，可直接用 AVX2 加载。
// is_view_ 决定各列 CopyFrom 的默认行为：
//   - true ：扫描时零拷贝指向底层数据（如 mmap 区）
//   - false：扫描时深拷贝到 BufferPool 缓冲
//
// 所有执行期 VectorBatch 必须绑定 BufferPool（buffer_pool_），
// 使各列的实际内存模式走 BufferPool 而非系统分配。
class VectorBatch {
  public:
    explicit VectorBatch(BufferPool* buffer_pool = nullptr, bool is_view = true);

    static constexpr uint32_t BATCH_SIZE = 1024;

    // ---------- 公共成员 ----------

    std::vector<ColumnData> columns;  // 各列数据
    std::vector<uint32_t> sel_vector; // 选择向量：有效行的 physical 局部索引
    uint32_t size = 0;                // 当前 batch 有效行数

    bool is_view() const;

    BufferPool* buffer_pool() const noexcept {
        return buffer_pool_;
    }

    // 添加一列
    void AddColumn(DataType type);

    // 重置 batch 状态（owned buffer 归还 BufferPool，减少分配）
    void Reset();

    // 按 sel_vector 就地压缩各列：只保留选中行，并更新 size。
    // 视图模式下压缩会隐式物化（选中行被拷贝进各列 BufferPool 缓冲），
    // 因为视图指向的外部内存不可写且不能只保留部分行。
    void CompactBySel();

    uint32_t ColumnCount() const;

  private:
    BufferPool* buffer_pool_ = nullptr;

    bool is_view_;
};
} // namespace simple_olap
