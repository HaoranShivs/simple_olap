# benchmark

性能基准测试。所有端到端计时都在 C++ benchmark 可执行文件内完成；
shell 脚本只负责 build / 生成数据集 / 汇总结果，不做纳秒级计时。

指标口径与整改方案见 `docs/BenchmarkSettingV2.md`。

## 指标定义

| 指标 | 口径 | 谁在测 |
| --- | --- | --- |
| `engine` QPS / 延迟 | `Connection::Execute(sql, consumer)`：SQL 前端 + planning + 执行 + O(1) blackhole consumer，不物化结果 | `bench_query_workload --result-mode engine` |
| `prepared` 延迟 | `Connection::Prepare(sql)` 只做一次，计时只包住 `Connection::Execute(prepared, consumer)`，即 execution-only | `bench_query_workload --result-mode prepared` |
| `materialized` QPS / 延迟 | `Connection::Query(sql)`：engine + `QueryResult` 全量深拷贝 | `bench_query_workload --result-mode materialized` |
| P50 / P95 / P99 | 每次 query 记录一个 `steady_clock` 样本，per-client 本地 vector，结束后 merge；nearest-rank `ceil(p*N)-1` | `bench_query_workload` |
| QPS | measurement window 内成功完成的 query 数 / 真实 elapsed 秒数（**不是** `1000 / latency`） | `bench_query_workload` |
| 扫描吞吐 | `input_rows/s` = 扫描物理行 / s（主指标），`output_rows/s` = filter 后 active rows / s，并给出 selectivity | `bench_parallel_scan` |
| 并行加速比 | `serial median / parallel median`（best 只作参考列） | `bench_parallel_scan` |
| 分配开销 | 测量窗口内 `BlockPool` / `BufferPool` 计数增量：`system_allocations/query`、`pool_hit_rate` | `bench_query_workload` |

正确性不再只比较 row count：所有 A/B 校验使用与行序无关的 `ResultDigest`
（`row_count + hash1 + hash2`，`src/main/result_digest.h`）。这样 multi 模式
输出顺序不同也不会产生 false mismatch。

## 端到端：bench_query_workload

固定数据集 `perf_data`（无索引，走 SeqScan）：

```
id BIGINT, group_low BIGINT, group_high BIGINT, value DOUBLE, value2 DOUBLE
```

固定 workload：Q1 storage compare / Q2 execution filter / Q3 sparse projection /
Q4 低基数 GROUP BY / Q5 高基数 GROUP BY / mixed 加权混合。

```bash
# 生成数据集
./build/bin/bench_query_workload --db benchmark/bench_data/perf_db --prepare --rows 4000000

# engine + materialized 两个口径（默认），4 clients
./build/bin/bench_query_workload --db benchmark/bench_data/perf_db \
    --workload mixed --clients 4 --result-mode both --warmup 5 --duration 30 --min-samples 5000

# execution-only（预编译）延迟
./build/bin/bench_query_workload --db benchmark/bench_data/perf_db \
    --workload q5 --result-mode prepared

# 内存消融 baseline
./build/bin/bench_query_workload --db benchmark/bench_data/perf_db --workload q5 \
    --memory system --buffer-mode direct --block-cache 0 --buffer-cache 0
```

要点：

- Database 只构造一次；每个 client 一个独立 `Connection`；start barrier 后统一
  测量；每 client 本地记录 latency，结束后 merge，不在热路径抢全局锁。
- 测量前自动对每条 workload 做 engine / prepared / materialized digest 一致性
  校验，任何不一致直接 abort。
- `query_count < 1000` 时输出 `[insufficient samples for p99]`：P99 不作为正式
  指标。正式结果建议 `min_samples >= 5000`。
- 启动时打印 commit / build type / compiler / CPU / 物理核数 / AVX2 /
  `vector_batch_rows`（固定 1024）等环境信息。
- `--load-batch` 控制的是**数据写入侧** DataChunk 行数；执行层
  `kVectorBatchSize = 1024` 是编译期常量，二者不是一回事。

## 一键矩阵：run_perf_suite.sh

```bash
./benchmark/run_perf_suite.sh                # 正式（耗时较长）
./benchmark/run_perf_suite.sh --quick        # 快速自检
./benchmark/run_perf_suite.sh --result-mode engine --skip-memory
./benchmark/run_perf_suite.sh --cpus 0-3     # 固定 CPU 集合（可选，需 taskset）
```

流程：

1. build；
2. 生成标准数据集 + latency profile 数据集；
3. correctness smoke（单元测试） + `verify_simd.sh` digest 校验；
4. QPS matrix：client 并发 × workload（single-thread execution）；
5. parallel scaling：`--mode multi --threads 1/2/4/8`；
6. AVX2 A/B：同一二进制，`SIMPLE_OLAP_FORCE_SCALAR=1` vs 默认，
   **paired 顺序**（每轮交替 scalar→avx2 / avx2→scalar）；
7. 内存消融 M0→M3 + 微基准原始输出。

内存消融矩阵（`--result-mode both` 时每个配置同时得到 engine / materialized 两行）：

| 配置 | memory | buffer-mode | block-cache | buffer-cache |
| --- | --- | --- | --- | --- |
| M0 | system | direct | 0 | 0 |
| M1 | arena | direct | 0 | 0 |
| M2 | arena | pooled | 64 | 0 |
| M3 | arena | pooled | 64 | 64 |

产出：

- `benchmark/results/perf_suite_<ts>.csv`：一行一个 (配置, result_mode)；
- `benchmark/results/summary_<ts>.txt`：按配置取 median QPS/P50/P99；
- `benchmark/results/micro_<ts>.txt`：五个微基准原始输出。

任何一次 measurement 失败都会中止整个 suite（不再 `|| true` 吞错误）。

## 正确性校验：verify_simd.sh

```bash
./benchmark/verify_simd.sh
./benchmark/verify_simd.sh --rows 1000000 --no-build --no-gen
./benchmark/verify_simd.sh --sql "SELECT COUNT(*) FROM bench_data WHERE value > 0.5;"
```

在同一二进制上比较 `(scalar, single)` / `(avx2, single)` / `(avx2, multi)`
三种组合的 `--digest` 摘要，覆盖 SIMD 后端与单/多线程路径。

## 并行扫描：bench_parallel_scan

```bash
./benchmark/run_bench.sh                                  # 包装脚本
./build/bin/bench_parallel_scan --rows 4000000 --threads 1,2,4
./build/bin/bench_parallel_scan --scenario filter --load-batch 4096
```

输出主指标 `input_rows/s`（物理扫描行）与 `output_rows/s`（filter 后 active
rows）以及 selectivity；`scan-only` 场景两者相同。speedup 使用 median。
`--batch` 保留为 `--load-batch` 的兼容别名。

注意：串行 `ScanCursor` 自持进度，benchmark 在计时区间外只 `Open` 一次，
串行/并行复用同一实例；每轮计时仍包含首次懒加载 mmap，用预热轮 + 多轮
median 削弱冷启动影响。纯零拷贝扫描会被并行队列/线程开销吃掉，
只有每行 CPU 工作占主导（filter / aggregate）时并行才有正收益。

## 微基准

```bash
./build/bin/bench_compare_kernel       # scalar / 64-row bitmap block / 旧逐段 |= 写法
./build/bin/bench_arith_kernel         # legacy variant vs scalar vs AVX2 算术内核
./build/bin/bench_sparse_projection    # mask gather vs 旧逐行物化（kernel-only / pipeline 两层）
./build/bin/bench_selection_pipeline   # mask-native vs selection-vector 往返
./build/bin/bench_global_aggregate     # hash 表路径 vs Global Fast Path（consume / full 两层）
./build/bin/bench_perf_counters        # perf_event_open: cycles/IPC/L1d/LLC/MPKI/branch-miss
```

统一 harness（`bench_common.h`）：

- 默认 3 轮 warmup + 15 个 sample，每个 sample 至少几毫秒（由 `--inner` 控制）；
- 多 kernel 对比使用固定种子随机顺序交错，消除固定 A/B 顺序偏差；
- 主指标是 median + MAD，不把 best 当主指标；
- 每个被测 kernel 内部不掺入额外 popcount / checksum 扫描；确需模拟下游
  消费时单独输出 pipeline 指标。

微基准的具体修正：

- `bench_arith_kernel`：浮点数据改用 `mt19937_64 + uniform_real_distribution`，
  修复旧实现中 double LCG 溢出成 `inf` 的问题；
- `bench_compare_kernel`：`SelectionMask::Count()` 完全移出 timed region；
- `bench_sparse_projection`：拆成 kernel-only（gather+arith）与 pipeline
  （+下游 checksum），局部 SIMD 收益看前者；
- `bench_global_aggregate`：拆成 `global-consume`（只计 Consume）与
  `global-full`（state 构造 + Consume + NextResult）；sink 同时消费 count 与
  sum，SUM-only 场景不会被优化掉。

## 尚未实现

- 压缩率（encoding ratio）benchmark 尚未实现。
