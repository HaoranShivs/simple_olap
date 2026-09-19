// ============================================================
// Global Aggregate 微基准：hash 表路径 vs Global Fast Path
// ============================================================
//
// 无 GROUP BY 时，旧实现仍然对每一行：
//   构造空 GroupKey -> pmr::unordered_map::try_emplace -> 逐行 UpdateAggregate
//
//   [legacy]  本地复刻旧 Consume：GroupKey + hash 表 + 逐行 COUNT(*)/SUM
//   [global]  HashAggregateState 的 GLOBAL 模式：
//               COUNT(*)      -> count += active rows（batch 级，一次）
//               其余聚合       -> 逐 active row 直接更新 global_states_（不查 hash）
//
// 覆盖三种调用组合：COUNT(*) / SUM(value) / COUNT(*) + SUM(value)。
// 先验证两条路径结果一致，再计时；输出 ns/row。
//
// 构建：
//   cmake --build build --target bench_global_aggregate
// 运行：
//   ./build/bin/bench_global_aggregate [--iters N] [--verify-only]

#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <memory_resource>
#include <string>
#include <unordered_map>
#include <vector>

#include "../src/execution/aggregate/hash_aggregate_state.h"
#include "../src/execution/batch_utils.h"
#include "../src/execution/expression/exec_expression.h"
#include "../src/execution/vector/vector.h"
#include "../src/memory/buffer_pool/buffer_pool.h"
#include "../src/type.h"

using namespace simple_olap;

namespace {

using Clock = std::chrono::steady_clock;

constexpr uint32_t kRowsPerBatch = 1024;
constexpr uint32_t kBatches = 64;
constexpr uint32_t kRows = kRowsPerBatch * kBatches;

struct Args {
    uint32_t iters = 3000;
    bool verify_only = false;
};

Args ParseArgs(int argc, char** argv) {
    Args a;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--iters" && i + 1 < argc) {
            a.iters = static_cast<uint32_t>(std::stoul(argv[++i]));
        } else if (arg == "--verify-only") {
            a.verify_only = true;
        }
    }
    return a;
}

template <typename Fn> double TimeNs(Fn&& fn, uint32_t iters) {
    const auto t0 = Clock::now();
    for (uint32_t i = 0; i < iters; ++i) {
        fn();
    }
    const auto t1 = Clock::now();
    return static_cast<double>(std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count()) /
           static_cast<double>(iters);
}

double ReferenceValue(uint32_t row) {
    return static_cast<double>(row) * 0.5 - 100.0;
}

// ---------- 旧路径复刻 ----------
//
// 与旧 HashAggregateState::Consume 一致：
//   每行构造空 GroupKey + hash 表 try_emplace，再逐行更新中间态。
struct LegacyGroupState {
    int64_t count = 0;
    long double sum = 0.0L;
};

struct LegacyResult {
    int64_t count = 0;
    long double sum = 0.0L;
};

LegacyResult ConsumeLegacy(const std::vector<VectorBatch>& batches, const ExecExpression* sum_arg, bool do_count,
                           bool do_sum) {
    std::pmr::unordered_map<HashAggregateState::GroupKey, LegacyGroupState, HashAggregateState::GroupKeyHash> groups{
        std::pmr::new_delete_resource()};

    for (const auto& batch : batches) {
        ForEachActiveRow(batch, [&](uint32_t row) {
            HashAggregateState::GroupKey key(std::pmr::new_delete_resource());
            auto [it, inserted] = groups.try_emplace(std::move(key), LegacyGroupState{});
            LegacyGroupState& state = it->second;
            if (do_count) {
                ++state.count;
            }
            if (do_sum) {
                const ExecValue value = sum_arg->Eval(batch, row);
                state.sum += ExecValueAsNumber(value);
            }
        });
    }

    LegacyResult result;
    for (const auto& [key, state] : groups) {
        result.count += state.count;
        result.sum += state.sum;
    }
    return result;
}

// ---------- 新路径（HashAggregateState::GLOBAL） ----------

LegacyResult ConsumeGlobalFast(const std::vector<ExecExprPtr>& group_exprs, const std::vector<AggCallSpec>& calls,
                               std::vector<VectorBatch>& batches) {
    HashAggregateState state(&group_exprs, &calls, std::pmr::new_delete_resource());
    for (VectorBatch& batch : batches) {
        state.Consume(batch);
    }

    LegacyResult result;
    // 从 global_states_ 读回（通过 NextResult，语义等价于最终输出）
    std::vector<AggregateOutputSpec> outputs;
    outputs.reserve(calls.size());
    for (uint32_t i = 0; i < static_cast<uint32_t>(calls.size()); ++i) {
        outputs.push_back({AggregateOutputSpec::Kind::AGGREGATE, i, calls[i].result_type, "v"});
    }
    state.set_outputs(&outputs);

    BufferPool pool;
    VectorBatch out(&pool, false);
    state.NextResult(out);
    for (uint32_t col = 0; col < static_cast<uint32_t>(out.columns.size()); ++col) {
        const ExecValue v = ReadExecValue(out.columns[col], 0);
        if (calls[col].type == AggType::COUNT) {
            result.count = std::get<int64_t>(v);
        } else {
            result.sum = ExecValueAsNumber(v);
        }
    }
    return result;
}

struct ScenarioResult {
    double legacy_ns_per_row = 0.0;
    double global_ns_per_row = 0.0;
};

volatile int64_t g_sink = 0;

ScenarioResult RunScenario(const char* name, const std::vector<ExecExprPtr>& group_exprs,
                           const std::vector<AggCallSpec>& calls, const ExecExpression* sum_arg, bool do_count,
                           bool do_sum, std::vector<VectorBatch>& batches, uint32_t iters, bool verify_only) {
    // 正确性校验
    const LegacyResult legacy = ConsumeLegacy(batches, sum_arg, do_count, do_sum);
    const LegacyResult fast = ConsumeGlobalFast(group_exprs, calls, batches);
    if (legacy.count != fast.count || std::fabs(static_cast<double>(legacy.sum - fast.sum)) > 1e-6) {
        std::printf("VERIFY FAILED: %s (legacy count=%lld sum=%Lf, fast count=%lld sum=%Lf)\n", name,
                    static_cast<long long>(legacy.count), legacy.sum, static_cast<long long>(fast.count), fast.sum);
        std::exit(1);
    }

    ScenarioResult result;
    if (verify_only) {
        return result;
    }

    const double legacy_ns = TimeNs(
        [&]() {
            const LegacyResult r = ConsumeLegacy(batches, sum_arg, do_count, do_sum);
            g_sink += r.count;
        },
        iters);
    const double fast_ns = TimeNs(
        [&]() {
            const LegacyResult r = ConsumeGlobalFast(group_exprs, calls, batches);
            g_sink += r.count;
        },
        iters);

    result.legacy_ns_per_row = legacy_ns / static_cast<double>(kRows);
    result.global_ns_per_row = fast_ns / static_cast<double>(kRows);
    return result;
}

} // namespace

int main(int argc, char** argv) {
    const Args args = ParseArgs(argc, argv);

    BufferPool pool;
    std::vector<VectorBatch> batches;
    batches.reserve(kBatches);
    for (uint32_t b = 0; b < kBatches; ++b) {
        batches.emplace_back(&pool, /*is_view=*/false);
        VectorBatch& batch = batches.back();
        batch.AddColumn(DataType::INT32);
        batch.AddColumn(DataType::DOUBLE);
        batch.columns[0].Resize(kRowsPerBatch);
        batch.columns[1].Resize(kRowsPerBatch);
        int32_t* ids = batch.columns[0].mutable_data<int32_t>();
        double* values = batch.columns[1].mutable_data<double>();
        for (uint32_t row = 0; row < kRowsPerBatch; ++row) {
            const uint32_t global_row = b * kRowsPerBatch + row;
            ids[row] = static_cast<int32_t>(global_row);
            values[row] = ReferenceValue(global_row);
        }
        batch.SetIdentitySelection(kRowsPerBatch);
    }

    const std::vector<ExecExprPtr> no_groups;

    std::vector<AggCallSpec> count_calls;
    count_calls.push_back(AggCallSpec{AggType::COUNT, nullptr, DataType::INT64});

    std::vector<AggCallSpec> sum_calls;
    sum_calls.push_back(
        AggCallSpec{AggType::SUM, std::make_unique<ExecColumnRef>(1, DataType::DOUBLE), DataType::DOUBLE});

    std::vector<AggCallSpec> both_calls;
    both_calls.push_back(AggCallSpec{AggType::COUNT, nullptr, DataType::INT64});
    both_calls.push_back(
        AggCallSpec{AggType::SUM, std::make_unique<ExecColumnRef>(1, DataType::DOUBLE), DataType::DOUBLE});

    const ExecExpression* sum_arg = both_calls[1].arg.get();

    std::printf("rows = %u (%u batches x %u), iters = %u\n\n", kRows, kBatches, kRowsPerBatch, args.iters);

    const ScenarioResult count_only =
        RunScenario("COUNT(*)", no_groups, count_calls, sum_arg, /*do_count=*/true, /*do_sum=*/false, batches,
                    args.iters, args.verify_only);
    const ScenarioResult sum_only =
        RunScenario("SUM(value)", no_groups, sum_calls, sum_arg, /*do_count=*/false, /*do_sum=*/true, batches,
                    args.iters, args.verify_only);
    const ScenarioResult both = RunScenario("COUNT(*) + SUM(value)", no_groups, both_calls, sum_arg, /*do_count=*/true,
                                            /*do_sum=*/true, batches, args.iters, args.verify_only);

    if (args.verify_only) {
        std::printf("verify: legacy == global fast path [OK]\n");
        return 0;
    }

    std::printf("%-24s %16s %16s %10s\n", "scenario", "legacy(ns/row)", "global(ns/row)", "speedup");
    std::printf("%s\n", std::string(70, '-').c_str());
    const ScenarioResult results[] = {count_only, sum_only, both};
    const char* names[] = {"COUNT(*)", "SUM(value)", "COUNT(*) + SUM(value)"};
    for (int i = 0; i < 3; ++i) {
        const double speedup =
            results[i].global_ns_per_row > 0.0 ? results[i].legacy_ns_per_row / results[i].global_ns_per_row : 0.0;
        std::printf("%-24s %16.3f %16.3f %9.2fx\n", names[i], results[i].legacy_ns_per_row,
                    results[i].global_ns_per_row, speedup);
    }

    return 0;
}
