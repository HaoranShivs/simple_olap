// Mask-native 执行链验证程序（手工编译或通过 CMake test target 构建）。
//
// 编译（在项目根目录）：
//   g++ -std=c++17 -O2 -Isrc test/mask_pipeline_test.cpp \
//       build/lib/libsimple_olap_core.a -lpthread -o /tmp/mask_pipeline_test
//
// 覆盖：
//   Filter -> Direct Projection（selection propagation / late materialization）
//   Filter -> Arithmetic Projection（sparse -> typed gather -> dense SIMD）
//   Filter -> Scalar Projection（SelectionMask 直接遍历）
//   Filter -> Filter
//   Filter -> Aggregate（mask iteration）
//   Dense 输入下的整列 SIMD 路径
//   VectorBatch::CompactBySelection
//
// 所有稀疏路径的结果都与逐行标量参考实现对比。

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <memory_resource>
#include <vector>

#include "execution/aggregate/hash_aggregate.h"
#include "execution/batch_utils.h"
#include "execution/expression/exec_expression.h"
#include "execution/filter/filter.h"
#include "execution/operator.h"
#include "execution/projection/projection.h"
#include "execution/vector/vector.h"
#include "memory/buffer_pool/buffer_pool.h"
#include "simd/selection_mask.h"
#include "type.h"

using namespace simple_olap;
using simple_olap::simd::SelectionMask;

namespace {

int g_failures = 0;

void Fail(const char* label) {
    std::printf("FAIL: %s\n", label);
    ++g_failures;
}

constexpr uint32_t kN = 1024;

// 产出一批预构造 batch 的 mock 叶子算子。
class MockScanOperator final : public Operator {
  public:
    explicit MockScanOperator(std::vector<VectorBatch> batches) : batches_(std::move(batches)) {}

    void Init() override {
        next_ = 0;
    }

    bool Next(VectorBatch& batch) override {
        if (next_ >= batches_.size()) {
            return false;
        }
        batch = std::move(batches_[next_++]);
        return true;
    }

  private:
    std::vector<VectorBatch> batches_;
    size_t next_ = 0;
};

// 确定性选择率掩码。
SelectionMask MakeMask(uint32_t percent) {
    SelectionMask mask;
    mask.SetNone(kN);
    for (uint32_t row = 0; row < kN; ++row) {
        if ((row * 2654435761u) % 100u < percent) {
            mask.data()[row >> 6] |= (uint64_t(1) << (row & 63));
        }
    }
    return mask;
}

// (id INT32, value DOUBLE) 两列 owned batch。
double ReferenceValue(uint32_t row) {
    return static_cast<double>(row) * 0.5 - 100.0;
}

VectorBatch MakeBatch(BufferPool* pool, const SelectionMask& selection) {
    VectorBatch batch(pool, /*is_view=*/false);
    batch.AddColumn(DataType::INT32);
    batch.AddColumn(DataType::DOUBLE);
    batch.columns[0].Resize(kN);
    batch.columns[1].Resize(kN);

    int32_t* ids = batch.columns[0].mutable_data<int32_t>();
    double* values = batch.columns[1].mutable_data<double>();
    for (uint32_t i = 0; i < kN; ++i) {
        ids[i] = static_cast<int32_t>(i);
        values[i] = ReferenceValue(i);
    }

    batch.SetSelection(selection);
    return batch;
}

ExecExprPtr ColRef(uint32_t slot, DataType type) {
    return std::make_unique<ExecColumnRef>(slot, type);
}

ExecExprPtr Literal(ExecValue value, DataType type) {
    return std::make_unique<ExecLiteral>(std::move(value), type);
}

ExecExprPtr Binary(PlanBinaryOp op, ExecExprPtr lhs, ExecExprPtr rhs, DataType type) {
    return std::make_unique<ExecBinary>(op, std::move(lhs), std::move(rhs), type);
}

// value OP constant（value 是 slot 1 的 DOUBLE 列）。
ExecExprPtr ValueBinary(PlanBinaryOp op, double constant) {
    return Binary(op, ColRef(1, DataType::DOUBLE), Literal(ExecValue{constant}, DataType::DOUBLE), DataType::DOUBLE);
}

// 消耗整个 pipeline，返回所有输出 batch。
std::vector<VectorBatch> Drain(Operator& root, BufferPool* pool) {
    root.Init();
    std::vector<VectorBatch> out;
    VectorBatch batch(pool, /*is_view=*/false);
    while (root.Next(batch)) {
        out.push_back(std::move(batch));
        batch = VectorBatch(pool, /*is_view=*/false);
    }
    return out;
}

// ---------- 测试 1：sparse -> gather -> AVX2 算术投影 ----------

void TestSparseArithmeticProjection(BufferPool* pool) {
    for (uint32_t percent : {1, 10, 50, 90, 99, 100}) {
        const SelectionMask mask = MakeMask(percent);
        std::vector<VectorBatch> batches;
        batches.push_back(MakeBatch(pool, mask));

        std::vector<ExecExprPtr> exprs;
        exprs.push_back(ValueBinary(PlanBinaryOp::ADD, 1.0));

        ProjectionOperator proj(std::make_unique<MockScanOperator>(std::move(batches)), std::move(exprs), pool);
        std::vector<VectorBatch> out = Drain(proj, pool);
        if (out.size() != 1) {
            Fail("sparse arithmetic: expected exactly one output batch");
            continue;
        }

        VectorBatch& output = out[0];
        if (!output.IsDense()) {
            Fail("sparse arithmetic: output must be dense after gather");
        }
        if (output.ActiveSize() != mask.Count()) {
            Fail("sparse arithmetic: active size mismatch");
        }
        if (output.columns.size() != 1 || output.columns[0].type != DataType::DOUBLE) {
            Fail("sparse arithmetic: output column shape mismatch");
            continue;
        }

        const double* got = output.columns[0].data<double>();
        uint32_t dense = 0;
        mask.ForEachSetBit([&](uint32_t row) {
            const double expected = ReferenceValue(row) + 1.0;
            if (got[dense] != expected) {
                Fail("sparse arithmetic: value mismatch");
            }
            ++dense;
        });
    }
}

// ---------- 测试 2：混合直接列 + 表达式列（共享 gather） ----------

void TestMixedProjection(BufferPool* pool) {
    const SelectionMask mask = MakeMask(25);
    std::vector<VectorBatch> batches;
    batches.push_back(MakeBatch(pool, mask));

    std::vector<ExecExprPtr> exprs;
    exprs.push_back(ColRef(0, DataType::INT32)); // 直接列
    exprs.push_back(ValueBinary(PlanBinaryOp::SUB, 2.0));
    exprs.push_back(ColRef(1, DataType::DOUBLE)); // 与表达式共享同一输入列的直接列

    ProjectionOperator proj(std::make_unique<MockScanOperator>(std::move(batches)), std::move(exprs), pool);
    std::vector<VectorBatch> out = Drain(proj, pool);
    if (out.size() != 1) {
        Fail("mixed projection: expected one output batch");
        return;
    }

    VectorBatch& output = out[0];
    if (output.ActiveSize() != mask.Count() || output.columns.size() != 3) {
        Fail("mixed projection: shape/size mismatch");
        return;
    }

    const int32_t* ids = output.columns[0].data<int32_t>();
    const double* sub = output.columns[1].data<double>();
    const double* raw = output.columns[2].data<double>();

    uint32_t dense = 0;
    mask.ForEachSetBit([&](uint32_t row) {
        if (ids[dense] != static_cast<int32_t>(row)) {
            Fail("mixed projection: direct id mismatch");
        }
        if (raw[dense] != ReferenceValue(row)) {
            Fail("mixed projection: direct value mismatch");
        }
        if (sub[dense] != ReferenceValue(row) - 2.0) {
            Fail("mixed projection: expression value mismatch");
        }
        ++dense;
    });
}

// ---------- 测试 3：Filter -> Direct Projection（selection 传播） ----------

void TestFilterDirectProjection(BufferPool* pool) {
    const SelectionMask input_mask = MakeMask(60);
    std::vector<VectorBatch> batches;
    batches.push_back(MakeBatch(pool, input_mask));

    auto predicate = ValueBinary(PlanBinaryOp::GT, 0.0);
    auto filter = std::make_unique<FilterOperator>(std::make_unique<MockScanOperator>(std::move(batches)),
                                                   std::move(predicate));

    std::vector<ExecExprPtr> exprs;
    exprs.push_back(ColRef(0, DataType::INT32));
    exprs.push_back(ColRef(1, DataType::DOUBLE));

    ProjectionOperator proj(std::move(filter), std::move(exprs), pool);
    std::vector<VectorBatch> out = Drain(proj, pool);
    if (out.size() != 1) {
        Fail("filter+direct projection: expected one batch");
        return;
    }

    VectorBatch& output = out[0];
    SelectionMask expected_mask;
    expected_mask.SetNone(kN);
    uint32_t expected_count = 0;
    input_mask.ForEachSetBit([&](uint32_t row) {
        if (ReferenceValue(row) > 0.0) {
            expected_mask.data()[row >> 6] |= (uint64_t(1) << (row & 63));
            ++expected_count;
        }
    });

    if (output.ActiveSize() != expected_count) {
        Fail("filter+direct projection: active size mismatch");
    }
    // 直接列投影必须保持 selection 传播，不得强制物化。
    for (uint32_t row = 0; row < kN; ++row) {
        if (output.selection().Test(row) != expected_mask.Test(row)) {
            Fail("filter+direct projection: selection was not propagated");
            break;
        }
    }

    // 列仍是完整物理列，按 physical row 访问。
    const int32_t* ids = output.columns[0].data<int32_t>();
    const double* values = output.columns[1].data<double>();
    input_mask.ForEachSetBit([&](uint32_t row) {
        if (ReferenceValue(row) > 0.0) {
            if (ids[row] != static_cast<int32_t>(row) || values[row] != ReferenceValue(row)) {
                Fail("filter+direct projection: value mismatch");
            }
        }
    });
}

// ---------- 测试 4：Filter -> Filter -> Arithmetic Projection ----------

void TestFilterChainProjection(BufferPool* pool) {
    const SelectionMask input_mask = MakeMask(80);
    std::vector<VectorBatch> batches;
    batches.push_back(MakeBatch(pool, input_mask));

    auto pred1 = ValueBinary(PlanBinaryOp::GT, -50.0);
    auto filter1 = std::make_unique<FilterOperator>(std::make_unique<MockScanOperator>(std::move(batches)),
                                                    std::move(pred1));

    auto pred2 = ValueBinary(PlanBinaryOp::LT, 50.0);
    auto filter2 = std::make_unique<FilterOperator>(std::move(filter1), std::move(pred2));

    std::vector<ExecExprPtr> exprs;
    exprs.push_back(ValueBinary(PlanBinaryOp::ADD, 3.0));

    ProjectionOperator proj(std::move(filter2), std::move(exprs), pool);
    std::vector<VectorBatch> out = Drain(proj, pool);
    if (out.size() != 1) {
        Fail("filter chain: expected one batch");
        return;
    }

    VectorBatch& output = out[0];
    uint32_t expected_count = 0;
    for (uint32_t row = 0; row < kN; ++row) {
        if (input_mask.Test(row) && ReferenceValue(row) > -50.0 && ReferenceValue(row) < 50.0) {
            ++expected_count;
        }
    }
    if (output.ActiveSize() != expected_count) {
        Fail("filter chain: active size mismatch");
    }

    const double* got = output.columns[0].data<double>();
    uint32_t dense = 0;
    for (uint32_t row = 0; row < kN; ++row) {
        if (input_mask.Test(row) && ReferenceValue(row) > -50.0 && ReferenceValue(row) < 50.0) {
            if (got[dense] != ReferenceValue(row) + 3.0) {
                Fail("filter chain: value mismatch");
            }
            ++dense;
        }
    }
}

// ---------- 测试 5：sparse 下的标量表达式投影 ----------

void TestSparseScalarProjection(BufferPool* pool) {
    const SelectionMask mask = MakeMask(30);
    std::vector<VectorBatch> batches;
    batches.push_back(MakeBatch(pool, mask));

    // (value + 1) + 2：嵌套算术无法编译成 VectorExpression -> 标量路径。
    auto inner = ValueBinary(PlanBinaryOp::ADD, 1.0);
    std::vector<ExecExprPtr> exprs;
    exprs.push_back(Binary(PlanBinaryOp::ADD, std::move(inner), Literal(ExecValue{double(2.0)}, DataType::DOUBLE),
                           DataType::DOUBLE));

    ProjectionOperator proj(std::make_unique<MockScanOperator>(std::move(batches)), std::move(exprs), pool);
    std::vector<VectorBatch> out = Drain(proj, pool);
    if (out.size() != 1) {
        Fail("sparse scalar projection: expected one batch");
        return;
    }

    VectorBatch& output = out[0];
    if (!output.IsDense() || output.ActiveSize() != mask.Count()) {
        Fail("sparse scalar projection: output shape mismatch");
        return;
    }

    const double* got = output.columns[0].data<double>();
    uint32_t dense = 0;
    mask.ForEachSetBit([&](uint32_t row) {
        if (got[dense] != ReferenceValue(row) + 3.0) {
            Fail("sparse scalar projection: value mismatch");
        }
        ++dense;
    });
}

// ---------- 测试 6：Filter -> Aggregate ----------

void TestFilterAggregate(BufferPool* pool) {
    const SelectionMask input_mask = MakeMask(40);
    std::vector<VectorBatch> batches;
    batches.push_back(MakeBatch(pool, input_mask));

    auto predicate = ValueBinary(PlanBinaryOp::GT, -20.0);
    auto filter = std::make_unique<FilterOperator>(std::make_unique<MockScanOperator>(std::move(batches)),
                                                   std::move(predicate));

    std::vector<AggCallSpec> agg_calls;
    agg_calls.push_back(AggCallSpec{AggType::COUNT, nullptr, DataType::INT64});
    agg_calls.push_back(AggCallSpec{AggType::SUM, ColRef(1, DataType::DOUBLE), DataType::DOUBLE});

    std::vector<AggregateOutputSpec> outputs;
    outputs.push_back(AggregateOutputSpec{AggregateOutputSpec::Kind::AGGREGATE, 0, DataType::INT64, "count"});
    outputs.push_back(AggregateOutputSpec{AggregateOutputSpec::Kind::AGGREGATE, 1, DataType::DOUBLE, "sum"});

    std::pmr::monotonic_buffer_resource memory;
    HashAggregateOperator agg(std::move(filter), /*group_exprs=*/{}, std::move(agg_calls), std::move(outputs), &memory,
                              pool);

    std::vector<VectorBatch> out = Drain(agg, pool);
    if (out.size() != 1 || out[0].ActiveSize() != 1) {
        Fail("filter+aggregate: expected one single-row batch");
        return;
    }

    const int64_t count = out[0].columns[0].data<int64_t>()[0];
    const double sum = out[0].columns[1].data<double>()[0];

    int64_t expected_count = 0;
    double expected_sum = 0.0;
    input_mask.ForEachSetBit([&](uint32_t row) {
        if (ReferenceValue(row) > -20.0) {
            ++expected_count;
            expected_sum += ReferenceValue(row);
        }
    });

    if (count != expected_count) {
        Fail("filter+aggregate: count mismatch");
    }
    if (std::fabs(sum - expected_sum) > 1e-9) {
        Fail("filter+aggregate: sum mismatch");
    }
}

// ---------- 测试 7：sparse GROUP BY（mask 行遍历） ----------

void TestSparseGroupBy(BufferPool* pool) {
    const SelectionMask mask = MakeMask(15);
    std::vector<VectorBatch> batches;
    batches.push_back(MakeBatch(pool, mask));

    std::vector<ExecExprPtr> group_exprs;
    group_exprs.push_back(ColRef(0, DataType::INT32));

    std::vector<AggCallSpec> agg_calls;
    agg_calls.push_back(AggCallSpec{AggType::COUNT, nullptr, DataType::INT64});
    agg_calls.push_back(AggCallSpec{AggType::SUM, ColRef(1, DataType::DOUBLE), DataType::DOUBLE});

    std::vector<AggregateOutputSpec> outputs;
    outputs.push_back(AggregateOutputSpec{AggregateOutputSpec::Kind::GROUP_KEY, 0, DataType::INT32, "id"});
    outputs.push_back(AggregateOutputSpec{AggregateOutputSpec::Kind::AGGREGATE, 0, DataType::INT64, "count"});
    outputs.push_back(AggregateOutputSpec{AggregateOutputSpec::Kind::AGGREGATE, 1, DataType::DOUBLE, "sum"});

    std::pmr::monotonic_buffer_resource memory;
    HashAggregateOperator agg(std::make_unique<MockScanOperator>(std::move(batches)), std::move(group_exprs),
                              std::move(agg_calls), std::move(outputs), &memory, pool);

    std::vector<VectorBatch> out = Drain(agg, pool);
    uint64_t total_rows = 0;
    for (const auto& batch : out) {
        total_rows += batch.ActiveSize();
    }
    if (total_rows != mask.Count()) {
        Fail("sparse group by: group count mismatch");
        return;
    }

    for (const auto& batch : out) {
        const int32_t* ids = batch.columns[0].data<int32_t>();
        const int64_t* counts = batch.columns[1].data<int64_t>();
        const double* sums = batch.columns[2].data<double>();
        for (uint32_t row = 0; row < batch.ActiveSize(); ++row) {
            if (counts[row] != 1) {
                Fail("sparse group by: per-group count mismatch");
            }
            const int32_t id = ids[row];
            if (sums[row] != ReferenceValue(static_cast<uint32_t>(id))) {
                Fail("sparse group by: per-group sum mismatch");
            }
            if (!mask.Test(static_cast<uint32_t>(id))) {
                Fail("sparse group by: produced group for unselected row");
            }
        }
    }
}

// ---------- 测试 8：CompactBySelection ----------

void TestCompactBySelection(BufferPool* pool) {
    const SelectionMask mask = MakeMask(35);
    VectorBatch batch = MakeBatch(pool, mask);

    batch.CompactBySelection();

    if (!batch.IsDense() || batch.ActiveSize() != mask.Count()) {
        Fail("compact: batch not dense after compaction");
        return;
    }

    const int32_t* ids = batch.columns[0].data<int32_t>();
    const double* values = batch.columns[1].data<double>();
    uint32_t dense = 0;
    mask.ForEachSetBit([&](uint32_t row) {
        if (ids[dense] != static_cast<int32_t>(row) || values[dense] != ReferenceValue(row)) {
            Fail("compact: value mismatch");
        }
        ++dense;
    });
}

} // namespace

int main() {
    BufferPool pool;
    TestSparseArithmeticProjection(&pool);
    TestMixedProjection(&pool);
    TestFilterDirectProjection(&pool);
    TestFilterChainProjection(&pool);
    TestSparseScalarProjection(&pool);
    TestFilterAggregate(&pool);
    TestSparseGroupBy(&pool);
    TestCompactBySelection(&pool);

    if (g_failures == 0) {
        std::printf("ALL MASK PIPELINE TESTS PASSED\n");
        return 0;
    }
    std::printf("%d FAILURE(S)\n", g_failures);
    return 1;
}
