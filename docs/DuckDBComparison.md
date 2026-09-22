# simple_olap vs DuckDB 公平对比报告

> 依据 `docs/Compare.md` 协议执行。原始数据：
> `benchmark/qps/results/medium_3engines_1_2_4_8.csv`、
> `benchmark/qps/results/large_3engines_1_4.csv`。

## 0. 结论速览

1. **架构重构目标达成（最重要）**：Medium / q2 / 4 线程下，Queue → Direct QPS 提升 **7.2x**
   （20.3 → 146.6），p99 从 **53.1 ms → 8.8 ms**，每次查询 context switch 从 **7019 → 6**。
   Queue 版本 1→8 线程 QPS 几乎不涨（24.8 → 21.4），Direct 版本扩展到 **4.6x**（52.4 → 241.4）。
2. **在能力交集的 7 条查询上，Simple Direct 与 DuckDB 各有胜负**：
   - 主指标 q2（扫描 + AVX2 过滤 + partial agg）：Simple Direct **1.19–1.36x DuckDB**；
   - q3（90% 过滤）：0.75–0.84；q4（1% 稀疏过滤）：**7.5–11.1x DuckDB**；
   - q1（SUM(double)）：0.27–0.36；q5（表达式聚合）：0.08–0.13；
   - q6（1024 组）、q7（65536 组）GROUP BY：0.07–0.59。
3. 剩余差距集中在 **row-wise 聚合内核**（q1 单线程 34 ms / 4.19M 行 ≈ 8 ns/行），
   调度层（morsel-driven workers）已经不再是瓶颈。
4. 观察到一个 DuckDB 自身的异常：Medium / q7 / 4 线程 反而比 2 线程慢（58–64 ms → 119–121 ms），
   `EXPLAIN ANALYZE` 显示 `HASH_GROUP_BY` 累计耗时从 0.07s 涨到 0.45s（可复现），
   大表（16.7M 行）下该异常消失。

## 1. 公平性设置

| 项目 | Simple Queue | Simple Direct | DuckDB |
|---|---|---|---|
| commit | `b096352` | `8d0906f` | `v1.5.5` (`d8cdaa33`) |
| 源码 | 本仓库 | 本仓库 | 官方仓库 clone + 官方 `scripts/amalgamation.py` |
| 编译 | `-O3`（AVX2 kernel 单独 `-mavx2`） | 同左 | `-O3 -DNDEBUG`，未开 `-march=native` |
| 并行模型 | scan 池 + compute 池 + `BoundedBlockingQueue` | pipeline worker + `ParallelScanGlobalState` | 原生 pipeline |
| 查询线程 | `--threads N`（scan=N, compute=N） | `--threads N`（worker=N） | `SET threads=N` |
| 结果顺序 | 不保证 | 不保证 | `SET preserve_insertion_order=false` |
| 内存 | 全内存 mmap | 全内存 mmap | `SET memory_limit='16GB'`，不 spill |

- 数据集逐行一致（两引擎各自生成、行内容相同）：
  `id=i, grp=i%1024, grp_hi=i%65536, value=((i*48271)%1000003)/1000003.0`
- Medium = 4,194,304 行（simple 64 segments；DuckDB 143 MB 压缩后），Large = 16,777,216 行。
- 计时协议：每个进程打开 DB → 逐查询 warmup 2s → 逐查询测量 5s（自动迭代数，非固定次数），
  **QPS = 完成查询数 / 总墙钟时间**；每次迭代把结果喂进 order-independent accumulator（防优化消除）。
- 正式计时前已用三方 `--verify` 校验结果一致（q5 的 SUM 类型不同但数值一致）。
- CPU：i5-12490F（4 物理核 + SMT = 8 逻辑核）；1/2/4 线程 `taskset -c 0-3`，8 线程 `taskset -c 0-7`。
- DuckDB 的 `core_functions`（v1.5 起 SUM/COUNT 所在扩展）按官方方式 `INSTALL/LOAD`，加载在计时之前。
- 局限：每格 1 轮（Compare.md 建议 5 轮）；未测 cold-cache 与 execution-only QPS-B；
  `perf` 在该环境不可用，用 `getrusage` 的 context switch 作为近似。

## 2. Medium（4.19M 行）threads=4 总表

| query | 说明 | Queue QPS | Direct QPS | DuckDB QPS | Direct/DuckDB | Queue p50 | Direct p50 | DuckDB p50 |
|---|---|---|---|---|---|---|---|---|
| q1 | SELECT SUM(value) | 19.2 | 88.2 | 317.1 | 0.28 | 52.05 ms | 11.20 ms | 3.19 ms |
| q2 | COUNT(*)+SUM WHERE value>0.5 | 20.3 | 146.6 | 123.1 | 1.19 | 49.69 ms | 6.69 ms | 8.05 ms |
| q3 | COUNT(*)+SUM WHERE value>0.1 | 19.5 | 90.8 | 120.5 | 0.75 | 51.41 ms | 10.82 ms | 8.28 ms |
| q4 | COUNT(*)+SUM WHERE value>0.99 | 148.0 | 1081.4 | 128.8 | 8.40 | 6.55 ms | 0.93 ms | 7.71 ms |
| q5 | SUM(id+1) | 18.4 | 57.7 | 589.8 | 0.10 | 54.62 ms | 17.27 ms | 1.68 ms |
| q6 | GROUP BY grp (1024) | 6.3 | 6.7 | 88.1 | 0.08 | 157.97 ms | 150.70 ms | 11.34 ms |
| q7 | GROUP BY grp_hi (65536) | 4.4 | 4.7 | 8.0 | 0.59 | 224.26 ms | 210.83 ms | 123.64 ms |

## 3. 架构 A/B：Queue → Direct（本次重构的直接收益）

### 3.1 QPS 提升

| query | 1 线程 | 2 线程 | 4 线程 | 8 线程 |
|---|---|---|---|---|
| q1 | 14.5 → 29.1 (2.0x) | 13.5 → 53.7 (4.0x) | 19.2 → 88.2 (4.6x) | 20.5 → 144.3 (7.1x) |
| q2 | 24.8 → 52.4 (2.1x) | 15.4 → 94.5 (6.1x) | 20.3 → 146.6 (7.2x) | 21.4 → 241.4 (11.3x) |
| q3 | 14.9 → 30.7 (2.1x) | 14.0 → 55.2 (4.0x) | 19.5 → 90.8 (4.7x) | 21.6 → 149.3 (6.9x) |
| q4 | 184.3 → 445.7 (2.4x) | 177.2 → 781.2 (4.4x) | 148.0 → 1081.4 (7.3x) | 26.4 → 1368.9 (51.8x) |
| q5 | 11.3 → 19.8 (1.7x) | 17.2 → 35.9 (2.1x) | 18.4 → 57.7 (3.1x) | 20.2 → 88.2 (4.4x) |
| q6 | 2.1 → 2.4 (1.1x) | 3.7 → 4.1 (1.1x) | 6.3 → 6.7 (1.1x) | 8.1 → 9.6 (1.2x) |
| q7 | 2.1 → 2.3 (1.1x) | 3.2 → 3.5 (1.1x) | 4.4 → 4.7 (1.1x) | 3.8 → 4.5 (1.2x) |

### 3.2 主指标 q2 的延迟稳定性（p99）

| 线程 | Queue p99 | Direct p99 | DuckDB p99 |
|---|---|---|---|
| 1 | 46.54 ms | 23.41 ms | 30.51 ms |
| 2 | 76.32 ms | 14.05 ms | 17.00 ms |
| 4 | 53.05 ms | 8.79 ms | 9.68 ms |
| 8 | 59.85 ms | 7.69 ms | 12.49 ms |

### 3.3 每次查询 context switch（q2，voluntary+involuntary）

| 线程 | Queue | Direct | DuckDB |
|---|---|---|---|
| 1 | 2391.94 | 2.05 | 0.01 |
| 2 | 5889.85 | 3.57 | 2.01 |
| 4 | 7019.46 | 6.17 | 13.89 |
| 8 | 7050.50 | 11.62 | 20.33 |

Queue 版本每次查询上千次上下文切换（每个 1024 行 batch 一次 futex 睡眠/唤醒），
Direct 版本降到个位数，与 QPS/p99 的改善方向完全一致——这正是重构要消灭的开销。

## 4. 并行扩展性（主指标 q2 QPS）

| 线程 | Queue | x | Direct | x | DuckDB | x |
|---|---|---|---|---|---|---|
| 1 | 24.8 | 1.00 | 52.4 | 1.00 | 38.5 | 1.00 |
| 2 | 15.4 | 0.62 | 94.5 | 1.81 | 71.4 | 1.85 |
| 4 | 20.3 | 0.82 | 146.6 | 2.80 | 123.1 | 3.19 |
| 8 | 21.4 | 0.86 | 241.4 | 4.61 | 180.8 | 4.69 |

Queue 版本并行度基本无效甚至负优化；Direct 与 DuckDB 的扩展曲线接近（8 线程分别 4.6x / 4.7x）。

## 5. Simple Direct / DuckDB QPS 比值（>1 表示 simple 更快）

| query | 1 线程 | 2 线程 | 4 线程 | 8 线程 |
|---|---|---|---|---|
| q1 | 0.27 | 0.27 | 0.28 | 0.36 |
| q2 | 1.36 | 1.32 | 1.19 | 1.34 |
| q3 | 0.82 | 0.80 | 0.75 | 0.84 |
| q4 | 11.10 | 10.35 | 8.40 | 7.52 |
| q5 | 0.08 | 0.08 | 0.10 | 0.13 |
| q6 | 0.08 | 0.08 | 0.08 | 0.07 |
| q7 | 0.20 | 0.21 | 0.59 | 0.40 |

## 6. Large（16.78M 行）关键查询

| query | 线程 | Queue QPS | Direct QPS | DuckDB QPS | Direct/DuckDB |
|---|---|---|---|---|---|
| q1 | 1 | 3.62 | 7.29 | 27.44 | 0.27 |
| q1 | 4 | 4.58 | 24.11 | 80.84 | 0.30 |
| q2 | 1 | 6.41 | 13.33 | 9.77 | 1.36 |
| q2 | 4 | 5.35 | 41.38 | 30.66 | 1.35 |
| q6 | 1 | 0.43 | 0.50 | 7.14 | 0.07 |
| q6 | 4 | 1.42 | 1.62 | 23.50 | 0.07 |
| q7 | 1 | 0.46 | 0.46 | 3.59 | 0.13 |
| q7 | 4 | 1.07 | 1.19 | 5.13 | 0.23 |

Large 规模结论与 Medium 一致：
- Queue → Direct 在扫描型查询 q1/q2 提升 2.0–7.7x，在聚合型 q6/q7 仅 1.0–1.2x；
- q2 仍由 Simple 领先（1.35x @4 线程），q1/q6/q7 由 DuckDB 领先；
- q6/q7 的 Direct QPS 相对 Medium 严格按行数线性下降（4x 行数 → 1/4 QPS），
  说明哈希聚合内核是稳定的瓶颈，并行度无法掩盖它。

## 7. DuckDB `EXPLAIN ANALYZE` 侧写（Medium, threads=4）

| query | Total | TABLE_SCAN（累计） | 聚合算子（累计） |
|---|---|---|---|
| q1 | 8.4 ms | 30 ms | UNGROUPED_AGGREGATE ≈ 0 |
| q2 | 11.8 ms | 40 ms | UNGROUPED_AGGREGATE ≈ 0 |
| q6 | 16.9 ms | 30 ms | PERFECT_HASH_GROUP_BY 30 ms |
| q7 | 132 ms | 30 ms | HASH_GROUP_BY 450 ms |

q7 的异常源于 `HASH_GROUP_BY`：4 线程时累计耗时是 2 线程（70 ms）的 6.4 倍，
导致 wall-clock 反而从 47.6 ms 涨到 132 ms。大表下未复现（1→4 线程 QPS 3.59→5.13）。
本报告只记录现象，不对 DuckDB 内部实现下结论。

## 8. 结论与下一步

1. **重构判断正确**：去掉 `BoundedBlockingQueue` + 两线程池后，
   扫描型查询在 4 线程下 QPS 提升 4.6–7.3x、p99 改善 4–6x、context switch 下降约 1000x，
   并行扩展从“无效”变为接近线性（4.6x @8 逻辑核）。
2. **与 DuckDB 的差距已定位到聚合内核**：
   - 扫描 + AVX2 过滤（q2/q3/q4）Simple 已不输甚至大幅领先（q4 7.5–11.1x）；
   - `SUM(double)`（q1/q5）和 `GROUP BY`（q6/q7）仍是 row-wise 标量更新，落后 3–14x；
   - 下一步优先级：向量化 SUM/COUNT 内核（q1/q5）→ 哈希聚合批量更新（q6/q7）→
     高基数 GROUP BY 的 radix partition + parallel merge。
3. **测量体系已落地**：`benchmark/qps/` 下统一 protocol 的三方 harness 可复用于后续任何 A/B。

## 9. VectorBatch 大小与队列设置核查

### 9.1 VectorBatch 大小

| 引擎 | 常量 | 值 | 位置 |
|---|---|---|---|
| simple_olap | `kVectorBatchSize` | **1024** | `src/common/constants.h:12`（`VectorBatch::BATCH_SIZE`；存储 `GetVectorBatch` 每次最多读 1024 行；`SelectionMask` = 16×u64 = 128 B） |
| DuckDB v1.5.5 | `STANDARD_VECTOR_SIZE` | **2048**（`DEFAULT_STANDARD_VECTOR_SIZE`） | `src/include/duckdb/common/vector_size.hpp:16`，编译期常量 |

两者**没有对齐**（1024 vs 2048）。为确认这是否影响结论，做了编译期敏感性测试
（Medium, threads=4，QPS）：

| query | simple 1024 | simple 2048 | duckdb 2048 | duckdb 1024 |
|---|---|---|---|---|
| q1 | 84.0 | 81.2 | 294.7 | 270.7 |
| q2 | 138.5 | 139.3 | 119.6 | 110.8 |
| q5 | 50.2 | 48.7 | 558.6 | 497.8 |
| q6 | 7.14 | 6.71 | 85.0 | 81.2 |
| q7 | 4.53 | 4.68 | 7.98 | 7.87 |

- simple_olap 1024→2048：差异在 ±6% 之内，无收益（SIMD/mask 按 `kVectorBatchSize` 自适应，2048 反而略差）；
- DuckDB 2048→1024：慢 4–11%（q1/q5 最明显）；
- 因此 batch size 差异（最多给 DuckDB ~10% 优势）**不足以解释 3–14x 的聚合差距**，也不改变任何排名结论。

### 9.2 队列设置

| 路径 | 队列 | 本次取值 |
|---|---|---|
| Simple Direct 聚合（q1–q7 全部） | **无任何队列**（worker 本地 partial agg → future → coordinator Merge） | — |
| Simple Direct 非聚合 SELECT | `result_queue_capacity`（最终结果交接） | 默认 64，本次未触发 |
| Simple Queue 聚合 | scan→compute `BoundedBlockingQueue` | 显式 64（`bench_simple_qps.cpp:169`；旧默认也是 64） |
| DuckDB | pipeline/morsel 队列为内部实现，无用户可见设置 | 仅 `SET threads` |

本次 7 条查询全部是聚合查询：Direct 侧队列容量不参与测量；Queue 侧用 64（比旧版
`bench_query_workload` 默认 16 更有利于旧架构，且前期实验显示调大容量也无法修复
空队列 futex 往返，故不存在“给旧架构用小队列”的偏受）。

