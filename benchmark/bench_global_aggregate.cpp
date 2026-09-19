// ============================================================
// Global Aggregate 微基准：hash 表路径 vs Global Fast Path
// ============================================================
//
// 无 GROUP BY 时，旧实现仍然对每一行：
//   构造空 GroupKey -> pmr::unordered_map::try_emplace -> 逐行 UpdateAggregate
//
//   [legacy]      本地复刻旧 Consume：GroupKey + hash 表 + 逐行 COUNT(*)/SUM
//   [global-consume]  HashAggregateState::GLOBAL 的纯 Consume（state 复用，
//                     不含 state 构造 / finalize / output 读取）
//   [global-full]     state 构造 + Consume + NextResult 输出（完整 fast path）
//
// 旧实现把两边 finalization 成本不对称地混在一起。现在显式拆成
//   consume_ns/row  —— 只用于评估 Consume 本身
//   full_ns/row     —— 用于评估完整算子成本
//
// sink 同时消费 count 与 sum，避免 SUM-only 场景里 count=0 让编译器把
// 整个 SUM 计算优化掉。
//
// 统一 harness：warmup + samples + median/MAD + 随机交错。
//
// 构建：
//   cmake --build build --target bench_global_aggregate
// 运行：
//   ./build/bin/bench_global_aggregate [--inner N] [--samples N] [--warmup N] [--verify-only]

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
#include "bench_common.h"

using namespace simple_olap;
using namespace simple_olap::bench;

namespace {

constexpr uint32_t kRowsPerBatch = 1024;
constexpr uint32_t kBatches = 64;
constexpr uint32_t kRows = kRowsPerBatch * kBatches;

struct Args {
    uint64_t inner = 200;
    uint32_t samples = 15;
    uint32_t warmup = 3;
    bool verify_only = false;
};

Args ParseArgs(int argc, char** argv) {
    Args a;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        const auto next = [&]() -> std::string {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "missing value for %s\n", arg.c_str());
                std::exit(1);
            }
            return argv[++i];
        };
        if (arg == "--inner" || arg == "--iters") {
            a.inner = std::stoull(next());
        } else if (arg == "--samples") {
            a.samples = static_cast<uint32_t>(std::stoul(next()));
        } else if (arg == "--warmup") {
            a.warmup = static_cast<uint32_t>(std::stoul(next()));
        } else if (arg == "--verify-only") {
            a.verify_only = true;
        } else {
            std::fprintf(stderr, "unknown argument: %s\n", arg.c_str());
            std::exit(1);
        }
    }
    return a;
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

std::vector<AggregateOutputSpec> MakeOutputs(const std::vector<AggCallSpec>& calls) {
    std::vector<AggregateOutputSpec> outputs;
    outputs.reserve(calls.size());
    for (uint32_t i = 0; i < static_cast<uint32_t>(calls.size()); ++i) {
        outputs.push_back({AggregateOutputSpec::Kind::AGGREGATE, i, calls[i].result_type, "v"});
    }
    return outputs;
}

LegacyResult ReadGlobalOutputs(HashAggregateState& state, const std::vector<AggregateOutputSpec>& outputs,
                               const std::vector<AggCallSpec>& calls, VectorBatch& out) {
    out.Reset();
    state.NextResult(out);

    LegacyResult result;
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

// consume-only：state 只构造一次并复用，inner 次调用全部只计入 Consume()。
// 循环结束后做一次 NextResult 读取结果（只占 sample 的 1/inner 成本），
// 用来阻止编译器把整个聚优化掉。
void ConsumeGlobalOnly(std::vector<VectorBatch>& batches, const std::vector<AggCallSpec>& calls,
                       const std::vector<AggregateOutputSpec>& outputs, uint64_t inner, BufferPool& pool) {
    const std::vector<ExecExprPtr> no_groups;
    HashAggregateState state(&no_groups, &calls, std::pmr::new_delete_resource());
    for (uint64_t i = 0; i < inner; ++i) {
        for (VectorBatch& batch : batches) {
            state.Consume(batch);
        }
    }

    state.set_outputs(&outputs);
    VectorBatch out(&pool, false);
    state.NextResult(out);
    DoNotOptimize(out.size);
}

// full：每次调用都完整走 state 构造 + Consume + NextResult。
LegacyResult ConsumeGlobalFull(std::vector<VectorBatch>& batches, const std::vector<AggCallSpec>& calls,
                               const std::vector<AggregateOutputSpec>& outputs, uint64_t inner, BufferPool& pool) {
    const std::vector<ExecExprPtr> no_groups;
    LegacyResult result;

    VectorBatch out(&pool, false);
    for (uint64_t i = 0; i < inner; ++i) {
        HashAggregateState state(&no_groups, &calls, std::pmr::new_delete_resource());
        for (VectorBatch& batch : batches) {
            state.Consume(batch);
        }
        state.set_outputs(&outputs);
        result = ReadGlobalOutputs(state, outputs, calls, out);
    }
    return result;
}

struct ScenarioResult {
    double legacy_full_ns_per_row = 0.0;
    double global_consume_ns_per_row = 0.0;
    double global_full_ns_per_row = 0.0;
};

void RunScenario(const char* name, const std::vector<AggCallSpec>& calls, const ExecExpression* sum_arg, bool do_count,
                 bool do_sum, std::vector<VectorBatch>& batches, BufferPool& pool, const Args& args) {
    // 正确性校验
    const LegacyResult legacy = ConsumeLegacy(batches, sum_arg, do_count, do_sum);
    const LegacyResult fast = ConsumeGlobalFull(batches, calls, MakeOutputs(calls), 1, pool);
    if (legacy.count != fast.count || std::fabs(static_cast<double>(legacy.sum - fast.sum)) > 1e-6) {
        std::printf("VERIFY FAILED: %s (legacy count=%lld sum=%Lf, fast count=%lld sum=%Lf)\n", name,
                    static_cast<long long>(legacy.count), legacy.sum, static_cast<long long>(fast.count), fast.sum);
        std::exit(1);
    }

    if (args.verify_only) {
        return;
    }

    const std::vector<AggregateOutputSpec> outputs = MakeOutputs(calls);

    const std::vector<MicroKernel> kernels = {
        {"legacy-full",
         [&](uint64_t inner) {
             LegacyResult result;
             for (uint64_t i = 0; i < inner; ++i) {
                 result = ConsumeLegacy(batches, sum_arg, do_count, do_sum);
             }
             // sink 同时消费 count 与 sum，SUM-only 场景也不会被优化掉。
             DoNotOptimize(result.count);
             DoNotOptimize(result.sum);
         }},
        {"global-consume",
         [&](uint64_t inner) {
             ConsumeGlobalOnly(batches, calls, outputs, inner, pool);
         }},
        {"global-full",
         [&](uint64_t inner) {
             const LegacyResult result = ConsumeGlobalFull(batches, calls, outputs, inner, pool);
             DoNotOptimize(result.count);
             DoNotOptimize(result.sum);
         }},
    };

    const std::vector<BenchmarkStats> stats =
        RunInterleavedMicroBenchmark(kernels, args.warmup, args.samples, args.inner);

    ScenarioResult result;
    result.legacy_full_ns_per_row = stats[0].median_ns / static_cast<double>(kRows);
    result.global_consume_ns_per_row = stats[1].median_ns / static_cast<double>(kRows);
    result.global_full_ns_per_row = stats[2].median_ns / static_cast<double>(kRows);

    std::printf("%-24s legacy-full=%8.4f  global-consume=%8.4f  global-full=%8.4f ns/row  (speedup full %.2fx)\n",
                name, result.legacy_full_ns_per_row, result.global_consume_ns_per_row,
                result.global_full_ns_per_row,
                result.global_full_ns_per_row > 0.0 ? result.legacy_full_ns_per_row / result.global_full_ns_per_row
                                                    : 0.0);
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

    std::printf("rows = %u (%u batches x %u)\n", kRows, kBatches, kRowsPerBatch);
    PrintEnvironment(QueryEnvironment());
    std::printf("inner = %llu, warmup = %u, samples = %u\n\n", static_cast<unsigned long long>(args.inner),
                args.warmup, args.samples);

    RunScenario("COUNT(*)", count_calls, sum_arg, /*do_count=*/true, /*do_sum=*/false, batches, pool, args);
    RunScenario("SUM(value)", sum_calls, sum_arg, /*do_count=*/false, /*do_sum=*/true, batches, pool, args);
    RunScenario("COUNT(*) + SUM(value)", both_calls, sum_arg, /*do_count=*/true, /*do_sum=*/true, batches, pool, args);

    if (args.verify_only) {
        std::printf("verify: legacy == global fast path [OK]\n");
    }
    return 0;
}
