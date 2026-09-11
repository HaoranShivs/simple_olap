#include "row_filter.h"

#include <stdexcept>

#include "../../simd/kernels.h"

namespace simple_olap {
namespace {

using simd::SelectionMask;

// 按 prepared predicate 的类型派发到对应的 compare kernel。
// 内核约定：先 SetNone(count)，只写 [0, count) 的位。
void RunTypedCompare(const PreparedStoragePredicate& predicate, const VectorBatch& batch, uint32_t count,
                     SelectionMask& out) {
    const ColumnData& column = batch.columns[predicate.output_slot];
    const simd::CompareKernels& kernels = simd::KernelRegistry::Instance().compare();

    switch (predicate.type) {
    case DataType::INT32:
        kernels.i32_const(column.data<int32_t>(), count, predicate.op, std::get<int32_t>(predicate.value), out);
        return;
    case DataType::INT64:
        kernels.i64_const(column.data<int64_t>(), count, predicate.op, std::get<int64_t>(predicate.value), out);
        return;
    case DataType::FLOAT:
        kernels.f32_const(column.data<float>(), count, predicate.op, std::get<float>(predicate.value), out);
        return;
    case DataType::DOUBLE:
        kernels.f64_const(column.data<double>(), count, predicate.op, std::get<double>(predicate.value), out);
        return;
    default:
        throw std::runtime_error("SIMD row filter encountered unsupported column type");
    }
}

} // namespace

void StorageRowFilter::Apply(const PreparedScanPredicates& prepared, const std::vector<uint8_t>& row_filter_mask,
                             VectorBatch& batch) {
    const uint32_t physical_count = batch.size;

    if (prepared.empty() || physical_count == 0) {
        batch.sel_vector.clear();
        batch.size = physical_count;
        return;
    }

    if (physical_count > kVectorBatchSize) {
        // SelectionMask 容量固定为 kVectorBatchSize；一旦 batch 超限，
        // SetAll 会静默截断导致漏行，因此这里直接报错而非产出错误结果。
        throw std::runtime_error("SIMD row filter: batch larger than SelectionMask capacity");
    }

    SelectionMask final_mask;
    final_mask.SetAll(physical_count);

    const auto& predicates = prepared.predicates();

    for (size_t p = 0; p < predicates.size(); ++p) {
        // metadata 已证明该 predicate 对本 segment ALL_MATCH：跳过
        if (p < row_filter_mask.size() && row_filter_mask[p] == 0) {
            continue;
        }

        const PreparedStoragePredicate& predicate = predicates[p];

        if (predicate.kind == PreparedStoragePredicate::Kind::ALWAYS_TRUE) {
            continue;
        }
        if (predicate.kind == PreparedStoragePredicate::Kind::ALWAYS_FALSE) {
            final_mask.SetNone(physical_count);
            break;
        }

        SelectionMask current;
        RunTypedCompare(predicate, batch, physical_count, current);
        final_mask.And(current);

        if (final_mask.Empty()) {
            break;
        }
    }

    if (final_mask.IsAll()) {
        // identity selection：全部行通过，无需 sel_vector
        batch.sel_vector.clear();
        batch.size = physical_count;
        return;
    }

    final_mask.ToSelectionVector(batch.sel_vector);
    batch.size = static_cast<uint32_t>(batch.sel_vector.size());
}

} // namespace simple_olap
