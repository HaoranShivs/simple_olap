#include "key_encoder.h"

#include <cstring>
#include <stdexcept>

#include "../../execution/vector/vector.h"

namespace simple_olap {

namespace {

// 追加一个定长列分量：类型标签 + 长度 + 原始字节
void AppendComponent(EncodedKey& out, DataType type, const uint8_t* data, size_t length) {
    out.push_back(static_cast<uint8_t>(type));
    const uint32_t len = static_cast<uint32_t>(length);
    for (uint8_t i = 0; i < 4; ++i) {
        out.push_back(static_cast<uint8_t>((len >> (i * 8)) & 0xFF));
    }
    out.insert(out.end(), data, data + length);
}

// VARCHAR 定长槽只编码有效内容（不含零填充），保持不同长度字符串编码不同
void AppendVarcharComponent(EncodedKey& out, const uint8_t* slot) {
    const uint32_t len = ReadVarcharLength(slot);
    AppendComponent(out, DataType::VARCHAR, slot + sizeof(uint32_t), len);
}

// 找到 column_id 在 schema.columns 中的下标；未找到属内部错误
size_t SchemaColumnIndex(const TableSchema& schema, ColumnId column_id) {
    for (size_t i = 0; i < schema.columns.size(); ++i) {
        if (schema.columns[i].column_id == column_id) {
            return i;
        }
    }
    throw std::runtime_error("key encoder: key column not found in schema");
}

// 找到 column_id 在 batch_columns 中的下标；键列必须已被请求读取
size_t BatchColumnIndex(const std::vector<ColumnId>& batch_columns, ColumnId column_id) {
    for (size_t i = 0; i < batch_columns.size(); ++i) {
        if (batch_columns[i] == column_id) {
            return i;
        }
    }
    throw std::runtime_error("key encoder: key column not present in batch");
}

// 字面量 -> 列类型转换后编码。
// 允许的安全拓宽：INT32 -> INT64、整数/浮点 -> DOUBLE、DOUBLE -> FLOAT。
void AppendScalarComponent(EncodedKey& out, DataType type, const ScalarValue& value) {
    switch (type) {
    case DataType::INT32: {
        if (const auto* v = std::get_if<int32_t>(&value)) {
            AppendComponent(out, type, reinterpret_cast<const uint8_t*>(v), sizeof(int32_t));
            return;
        }
        break;
    }
    case DataType::INT64: {
        if (const auto* v = std::get_if<int64_t>(&value)) {
            AppendComponent(out, type, reinterpret_cast<const uint8_t*>(v), sizeof(int64_t));
            return;
        }
        if (const auto* v = std::get_if<int32_t>(&value)) {
            const int64_t widened = *v;
            AppendComponent(out, type, reinterpret_cast<const uint8_t*>(&widened), sizeof(int64_t));
            return;
        }
        break;
    }
    case DataType::FLOAT: {
        if (const auto* v = std::get_if<float>(&value)) {
            AppendComponent(out, type, reinterpret_cast<const uint8_t*>(v), sizeof(float));
            return;
        }
        if (const auto* v = std::get_if<double>(&value)) {
            const float narrowed = static_cast<float>(*v);
            AppendComponent(out, type, reinterpret_cast<const uint8_t*>(&narrowed), sizeof(float));
            return;
        }
        break;
    }
    case DataType::DOUBLE: {
        if (const auto* v = std::get_if<double>(&value)) {
            AppendComponent(out, type, reinterpret_cast<const uint8_t*>(v), sizeof(double));
            return;
        }
        if (const auto* v = std::get_if<float>(&value)) {
            const double widened = *v;
            AppendComponent(out, type, reinterpret_cast<const uint8_t*>(&widened), sizeof(double));
            return;
        }
        if (const auto* v = std::get_if<int32_t>(&value)) {
            const double widened = *v;
            AppendComponent(out, type, reinterpret_cast<const uint8_t*>(&widened), sizeof(double));
            return;
        }
        if (const auto* v = std::get_if<int64_t>(&value)) {
            const double widened = static_cast<double>(*v);
            AppendComponent(out, type, reinterpret_cast<const uint8_t*>(&widened), sizeof(double));
            return;
        }
        break;
    }
    case DataType::VARCHAR: {
        if (const auto* v = std::get_if<std::string>(&value)) {
            AppendComponent(out, type, reinterpret_cast<const uint8_t*>(v->data()), v->size());
            return;
        }
        break;
    }
    default:
        break;
    }
    throw std::runtime_error("key encoder: literal type does not match key column type");
}

} // namespace

size_t EncodedKeyHash::operator()(const EncodedKey& key) const noexcept {
    // FNV-1a：短键上的分布足够，且无额外分配
    uint64_t hash = 1469598103934665603ULL;
    for (uint8_t byte : key) {
        hash ^= byte;
        hash *= 1099511628211ULL;
    }
    return static_cast<size_t>(hash);
}

EncodedKey KeyEncoder::Encode(const TableSchema& schema, const KeySchema& key, const DataChunk& chunk, uint32_t row) {
    EncodedKey out;

    for (ColumnId column_id : key.columns) {
        const ColumnSchema* column = FindColumnSchema(schema, column_id);
        if (column == nullptr) {
            throw std::runtime_error("key encoder: key column not found in schema");
        }

        const size_t index = SchemaColumnIndex(schema, column_id);
        const size_t elem_size = chunk.element_size(index);
        const uint8_t* cell = chunk.raw_data(index) + static_cast<size_t>(row) * elem_size;

        if (column->type == DataType::VARCHAR) {
            AppendVarcharComponent(out, cell);
        } else {
            AppendComponent(out, column->type, cell, elem_size);
        }
    }

    return out;
}

EncodedKey KeyEncoder::Encode(const TableSchema& schema, const KeySchema& key,
                              const std::vector<ColumnId>& batch_columns, const VectorBatch& batch, uint32_t row) {
    EncodedKey out;

    for (ColumnId column_id : key.columns) {
        const ColumnSchema* column = FindColumnSchema(schema, column_id);
        if (column == nullptr) {
            throw std::runtime_error("key encoder: key column not found in schema");
        }

        const size_t index = BatchColumnIndex(batch_columns, column_id);
        const ColumnData& column_data = batch.columns[index];
        const size_t elem_size = TypeElemSize(column_data.type);
        const uint8_t* cell = column_data.buffer + static_cast<size_t>(row) * elem_size;

        if (column->type == DataType::VARCHAR) {
            AppendVarcharComponent(out, cell);
        } else {
            AppendComponent(out, column->type, cell, elem_size);
        }
    }

    return out;
}

EncodedKey KeyEncoder::Encode(const TableSchema& schema, const KeySchema& key, const std::vector<ScalarValue>& values) {
    if (values.size() != key.columns.size()) {
        throw std::runtime_error("key encoder: lookup value count does not match key column count");
    }

    EncodedKey out;
    for (size_t i = 0; i < key.columns.size(); ++i) {
        const ColumnSchema* column = FindColumnSchema(schema, key.columns[i]);
        if (column == nullptr) {
            throw std::runtime_error("key encoder: key column not found in schema");
        }
        AppendScalarComponent(out, column->type, values[i]);
    }
    return out;
}

} // namespace simple_olap
