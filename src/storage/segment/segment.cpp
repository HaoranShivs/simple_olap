#include "segment.h"

#include <cstring>
#include <stdexcept>

#include "../file/filewriter.h"


namespace simple_olap
{
    // ==========================================
    // SegmentReader - 基于 mmap 的只读 segment
    // ==========================================
    // 磁盘文件格式：
    //   [SegmentMeta 序列化区][列0数据][列1数据]...
    // Open 时将整个文件 mmap 进内存，元数据从头部反序列化，
    // 各列的 ColumnChunkReader 直接指向映射区域内的 data_offset 处，零拷贝。

    SegmentReader::SegmentReader(uint64_t segment_id, SegmentMeta metadata, MappedFile file)
        : segment_id_(segment_id), metadata_(std::move(metadata)), mapped_file_(std::move(file))
    {
    }

    SegmentReader::~SegmentReader() = default;

    SegmentReader *SegmentReader::Open(SegmentId id, const std::filesystem::path &root_dir)
    {
        // 1. 定位 segment 文件（命名格式固定：root_dir / {id}）
        const std::filesystem::path seg_path = root_dir / std::to_string(id);

        // 2. mmap 只读映射整个文件
        auto mapped = MappedFile::OpenReadOnly(seg_path);
        if (!mapped.has_value())
        {
            return nullptr;
        }

        // 3. 从映射区头部反序列化 SegmentMeta
        SegmentMeta meta;
        try
        {
            BinaryReader reader(reinterpret_cast<const uint8_t *>(mapped->data()),
                                mapped->size());
            meta = SegmentMeta::Deserialize(reader);
        }
        catch (const std::exception &)
        {
            // 元数据损坏或文件不完整
            return nullptr;
        }

        if (meta.segment_id != id)
        {
            // 文件内容与请求的 id 不一致
            return nullptr;
        }

        // 4. 构造 reader，并为每列建立指向映射区的 ColumnChunkReader 视图（零拷贝）
        auto *self = new SegmentReader(id, std::move(meta), std::move(*mapped));

        const std::byte *base = self->mapped_file_.data();
        for (const auto &chunk_meta : self->metadata_.col_chunk_metas_)
        {
            const uint8_t *col_data =
                reinterpret_cast<const uint8_t *>(base + chunk_meta.data_offset);
            self->columns_.emplace(
                chunk_meta.column_id,
                ColumnChunkReader::CreateFromBuilder(
                    chunk_meta, col_data, self->metadata_.row_count,
                    TypeElemSize(chunk_meta.type)));
        }

        return self;
    }

    uint64_t SegmentReader::id() const
    {
        return segment_id_;
    }

    uint32_t SegmentReader::row_count() const
    {
        return metadata_.row_count;
    }

    const ColumnChunkMeta &SegmentReader::GetColumnMeta(ColumnId id) const noexcept
    {
        for (const auto &chunk : metadata_.col_chunk_metas_)
        {
            if (chunk.column_id == id)
            {
                return chunk;
            }
        }
        // 未找到：返回首个作为兜底（调用方应保证 id 有效）
        return metadata_.col_chunk_metas_.front();
    }

    ColumnChunkReader SegmentReader::OpenColumn(uint32_t column_id) const
    {
        auto it = columns_.find(column_id);
        if (it != columns_.end())
        {
            return it->second;
        }
        // 未找到列：返回空视图
        return ColumnChunkReader::CreateFromBuilder(ColumnChunkMeta{}, nullptr, 0, 0);
    }

    // ==========================================
    // SegmentBuilder - 写入侧
    // ==========================================

    SegmentBuilder::SegmentBuilder(const TableSchema &schema)
        : row_count_(0)
    {
        // 为 schema 中的每一列创建一个 ColumnBuilder
        column_builders_.reserve(schema.columns.size());
        for (const auto &col : schema.columns)
        {
            ColumnChunkMeta chunk_meta;
            chunk_meta.column_id = col.column_id;
            chunk_meta.type = col.type;
            chunk_meta.data_offset = 0;
            column_builders_.emplace_back(chunk_meta);
        }
    }

    void SegmentBuilder::Append(const DataChunk &batch)
    {
        const uint32_t row_count = static_cast<uint32_t>(batch.size());

        // 为每一列构造 ColumnVector 视图，交给对应的 ColumnBuilder
        for (size_t i = 0; i < column_builders_.size(); ++i)
        {
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
    bool SegmentBuilder::Flush(const std::filesystem::path &path)
    {
        // segment id 从文件名解析（命名格式固定：{segment_id}）
        uint32_t segment_id = 0;
        try
        {
            segment_id = static_cast<uint32_t>(std::stoul(path.filename().string()));
        }
        catch (const std::exception &)
        {
            return false;
        }

        // 1. 组装元数据（先以 data_offset=0 序列化一次，确定元数据区大小）
        SegmentMeta meta;
        meta.segment_id = segment_id;
        meta.row_count = row_count_;
        meta.path = path.filename(); // 只存文件名，避免绝对路径耦合
        meta.col_chunk_metas_.reserve(column_builders_.size());
        for (const auto &builder : column_builders_)
        {
            meta.col_chunk_metas_.push_back(builder.GetMeta());
        }

        BinaryWriter probe;
        meta.Serialize(probe);
        const uint64_t meta_size = probe.GetBuffer().size();

        // 2. 按列顺序计算各列数据在文件中的偏移，并回填元数据
        uint64_t offset = meta_size;
        for (size_t i = 0; i < column_builders_.size(); ++i)
        {
            meta.col_chunk_metas_[i].data_offset = offset;
            offset += static_cast<uint64_t>(column_builders_[i].row_count()) *
                      column_builders_[i].element_size();
        }

        // 回填后重新序列化（data_offset 为定长字段，总大小与 probe 一致）
        BinaryWriter meta_writer;
        meta.Serialize(meta_writer);
        const auto &meta_buf = meta_writer.GetBuffer();

        // 3. 写文件：元数据区 + 各列数据区（数据区交给 ColumnBuilder 自己写）
        FileWriter file;
        if (!file.Open(path))
        {
            return false;
        }

        if (!file.Write(meta_buf.data(), meta_buf.size()))
        {
            return false;
        }

        // 校验实际写入位置与预计算的布局一致（即元数据区大小未变）
        if (file.Tell() != meta_size)
        {
            return false;
        }

        for (auto &builder : column_builders_)
        {
            if (!builder.Flush(file))
            {
                return false;
            }
        }

        return file.Close();
    }

} // namespace simple_olap