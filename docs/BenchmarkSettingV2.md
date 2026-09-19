我把当前 `master` 的 benchmark 体系基本完整过了一遍。基于当前提交 `6569fdf...`，结论是：**不是所有 benchmark 都完全失效，但目前几乎所有“宏观性能指标”的测量边界都需要整改；几个微基准也存在不同程度的方法学问题。** 尤其是 QPS、P99、端到端延迟、整体 AVX2 收益、内存池/Arena 对 P99 的收益，目前都还不能作为严谨的简历数据。

### 现有指标审查

| 指标 / benchmark                | 当前状态              | 问题严重度 | 主要问题                                                          |
| ----------------------------- | ----------------- | ----: | ------------------------------------------------------------- |
| QPS / queries/s               | **实际上未实现**        |    🔴 | README 写了 queries/s，但当前没有真正持续并发压测器                            |
| P50/P95/P99                   | **实际上未实现**        |    🔴 | README 声明有，但没有 latency sample collector；5 次 repeat 更不可能计算 P99 |
| `bench_execution_modes.sh` 延迟 | 不宜叫 Query latency |    🔴 | 每个 sample 都重新启动进程、加载 DB、创建线程池                                 |
| single/multi speedup          | 有参考价值但污染严重        |    🟠 | 包含 process startup；multi 每次还会创建 scan ThreadPool               |
| `bench_parallel_scan` rows/s  | filter 场景定义有误     |    🔴 | 分子是 `ActiveRowCount()`，即过滤后的输出行，而不是实际扫描输入行                    |
| parallel scan speedup         | 可参考               |    🟠 | 用 `best` 算 speedup/rows/s，偏乐观；每轮 parallel 创建线程池               |
| 整体 AVX2 speedup               | 方向可参考，数字不严谨       |    🔴 | shell 启进程 + SQL frontend + QueryResult 深拷贝都混进去了               |
| Filter SIMD speedup           | 局部趋势可参考           |    🟠 | 同上，而且 benchmark 忽略执行失败                                        |
| Projection SIMD speedup       | 局部趋势可参考           |    🟠 | 同上，结果物化会严重掩盖 projection kernel 收益                             |
| arithmetic microbench         | 有问题               |    🟠 | 浮点测试数据生成存在明显 bug，后面大量值会溢出成 `inf`                              |
| compare microbench            | 基本思路正确            |    🟡 | 每次 kernel 后都执行 `mask.Count()`，把额外 popcount 成本混入 kernel        |
| sparse projection microbench  | 有明显污染             |    🟠 | timed region 内又完整遍历一次输出做 sum，可能掩盖 gather/AVX2 收益              |
| global aggregate microbench   | 有明显污染             |    🟠 | 新旧路径 finalization/BufferPool 成本不对称；SUM-only 的 sink 也不够稳健      |
| selection pipeline microbench | 可做趋势参考            |    🟡 | 没有 warmup、多 sample、随机 A/B 顺序；volatile sink 对极短操作影响明显          |
| Arena / BufferPool P99        | **没有实现**          |    🔴 | 只有 allocator counters，没有严格 A/B benchmark                      |
| system allocation 降低          | 部分基础设施已有          |    🟡 | 有 `system_allocations/pool_hits`，但没人做 measurement delta       |
| L3/LLC Cache Miss             | **没有实现**          |    🔴 | 文档只是建议 `perf stat`，benchmark 未实现                              |
| 压缩率                           | **没有实现**          |    🔴 | README 声明了指标，当前 benchmark 没有对应实现                              |

所以你之前觉得“QPS 怎么这么低”，现在我会更明确一点：

> **目前这个 QPS 数字首先应该怀疑测量方法，而不能首先归因于数据库执行引擎。**

---

# 一、先把性能指标重新定义

后面所有 benchmark 必须统一这套定义，否则不同数据不能互相比较。

### 1. Startup latency

单独保留：

```text
Cold Startup Latency
=
process creation
+ Database construction
+ Catalog load
+ ThreadPool creation
+ exit
```

这不是 SQL latency。

现有 shell benchmark 可以继续承担这一项，名称改成：

```text
startup_latency_ms
```

不要再从它推导 QPS。

---

### 2. Steady SQL latency

定义为：

```text
Lexer
→ Parser
→ Binder
→ Logical Planner
→ Optimizer
→ Physical Planner
→ Execution
```

前提：

```text
进程已经启动
Database 已经存在
ThreadPool 已经存在
OS page cache 已预热
```

计时：

```cpp
auto begin = steady_clock::now();

connection.Execute(sql, consumer);

auto end = steady_clock::now();
```

**不能由 shell 的 `date +%s%N` 包整个进程。**

输出：

```text
p50
p95
p99
mean
min
max
```

---

### 3. Execution-only latency

再额外定义：

```text
PhysicalPlan 已经生成
↓
ExecutionEngine
↓
Operator Tree
↓
结果 consumer
```

这一项用来分析：

> 查询执行引擎到底有多快。

因此需要增加：

```cpp
PreparedQuery Connection::Prepare(std::string_view sql);

ExecutionSummary Connection::Execute(
    const PreparedQuery& query,
    BatchConsumer consumer);
```

于是能分别测：

```text
SQL steady latency
Prepared execution latency
```

两者差值大体就是：

```text
SQL frontend + planning
```

这对之后定位短查询 QPS 非常重要。

---

# 二、重写 QPS benchmark

新增：

```text
benchmark/bench_query_workload.cpp
```

不再用 bash 循环执行程序。

核心生命周期必须变成：

```text
main
 │
 ├─ Database database(...)          ← 只创建一次
 │
 ├─ Connection[]                    ← 每 client 一个
 │
 ├─ warmup
 │
 ├─ START BARRIER
 │
 ├─ 30s steady measurement
 │    ├─ query
 │    ├─ query
 │    ├─ query
 │    └─ ...
 │
 └─ statistics
```

QPS 的严格定义：

```text
QPS =
measurement window 内成功完成的 query 数
----------------------------------------
真实 measurement elapsed seconds
```

不要：

```text
QPS = 1000 / 一次 SQL latency
```

因为并发情况下这两个根本不是一个概念。

测试 concurrency：

```text
1
2
4
8
physical_core_count
```

每个客户端必须拥有自己的：

```cpp
Connection
```

共享：

```cpp
Database
```

不要多个 client 共用一个 Connection。

---

# 三、QPS 同时必须测两种结果模式

你现在 `Connection::Query()` 一定会：

```cpp
result.Append(batch, &database_.GetBufferPool());
```

然后 `QueryResult::Append()`：

```cpp
column.CopyFrom(..., false);
```

也就是把整个 SELECT 结果再次深拷贝。

因此必须拆成两种指标。

### Engine QPS

```text
SQL
→ Execute
→ BatchConsumer
→ blackhole
```

不保存完整结果。

建议 API：

```cpp
struct ExecutionSummary {
    uint64_t row_count;
    uint64_t affected_rows;
};

ExecutionSummary Connection::Execute(
    std::string_view sql,
    const BatchConsumer& consumer);
```

benchmark consumer：

```cpp
uint64_t checksum = 0;

auto consumer = [&](const VectorBatch& batch,
                    const ExecSchema&) {
    checksum += ActiveRowCount(batch);
};
```

查询结束后再把 checksum 喂给 benchmark sink。

---

### Materialized QPS

保留：

```cpp
Connection::Query()
```

即：

```text
Execution
+
QueryResult materialization
```

最终结果必须同时报告：

| Metric             | 含义                |
| ------------------ | ----------------- |
| `engine_qps`       | 数据库执行能力           |
| `materialized_qps` | 当前完整 Query API 能力 |
| `engine_p99`       | 执行延迟              |
| `materialized_p99` | 包含结果深拷贝的延迟        |

这样 QueryResult 的代价就不会隐藏起来。

---

# 四、P50/P95/P99 必须重新实现

你现在 benchmark 默认：

```text
REPEATS=5
```

这种数据量根本不能谈 P99。

新 benchmark 每一次 Query 都记录：

```cpp
std::vector<uint64_t> latency_ns;
```

每 client 使用自己的 vector，结束后 merge，避免测量热路径中抢一把全局 mutex。

percentile 明确定义为 nearest-rank：

```cpp
index = ceil(p * N) - 1;
```

建议：

```text
warmup >= 5 s

measurement >= 30 s

且：
latency samples >= 5000 优先
```

最低要求：

```text
N < 1000
→ 不报告 P99
→ 输出 "insufficient samples"
```

否则一个所谓 P99 很容易只是噪声。

---

# 五、`bench_parallel_scan` 有一个实际的指标定义错误

当前：

```cpp
while (table->Scan(...)) {
    rows += ActiveRowCount(batch);
}
```

filter：

```sql
value > 0.5
```

大概只剩 50%。

因此你输出的：

```text
rows/s
```

实际上是：

```text
output rows / second
```

而不是：

```text
scanned rows / second
```

这对于 scan benchmark 是不正确的。

例如：

```text
扫描 4M rows
输出 2M rows
耗时 20 ms
```

目前显示：

```text
100M rows/s
```

实际上扫描吞吐是：

```text
200M input rows/s
```

### 必须增加 ScanMetrics

建议：

```cpp
struct ScanMetrics {
    uint64_t physical_rows_scanned = 0;
    uint64_t active_rows_emitted = 0;

    uint64_t segments_considered = 0;
    uint64_t segments_pruned = 0;

    uint64_t batches_produced = 0;
};
```

在 `ScanSegment()` 中：

```cpp
metrics.physical_rows_scanned += physical_rows;
metrics.active_rows_emitted += output.size;
```

最终同时输出：

```text
input_rows/s
output_rows/s
selectivity
```

主指标是：

```text
input_rows/s
```

---

# 六、parallel speedup 不应该再使用 best

当前：

```cpp
rows_per_s = rows / best_ms;
speedup = serial_best_ms / parallel_best_ms;
```

这是很容易“挑最好的一次”。

改成：

```text
primary:
median_ms

throughput:
rows / median_ms

speedup:
serial_median / parallel_median
```

`best` 可以保留，只作为参考：

```text
min
median
mean
p95
```

但不能作为 README 或简历上的主要性能数字。

---

# 七、还有一个很容易误解的参数：`--batch`

现在这些 benchmark 的：

```text
--batch 8192
```

实际上控制的是：

```cpp
DataChunk 写入数据集时的 batch
```

而不是查询执行的 VectorBatch。

真正查询 batch 是：

```cpp
kVectorBatchSize = 1024;
```

所以现在你运行：

```bash
--batch 1024
--batch 8192
```

并不能验证：

> 1024 vs 8192 vectorized execution batch size。

建议立即改名：

```text
--load-batch
```

例如：

```text
--load-batch 8192
```

真正执行 batch 暂时显示：

```text
vector_batch_rows = 1024 (compile-time)
```

以后如果支持 runtime batch size，再叫：

```text
--vector-batch
```

---

# 八、SIMD overall benchmark 整体需要改

当前：

```text
bench_simd.sh
bench_filter_simd.sh
bench_projection_simd.sh
```

全部存在一个相同的问题：

```text
scalar sample:
启动新进程
→ Database
→ ThreadPool
→ SQL
→ result copy
→ destroy

simd sample:
启动新进程
→ Database
→ ThreadPool
→ SQL
→ result copy
→ destroy
```

所以 SIMD 差异被大量无关成本稀释。

并且所谓 warmup：

```bash
for warmup
    time_once
```

每一次也都是一个**新的进程**。

因此它只对：

```text
OS page cache
```

有一定预热作用。

它根本不能预热：

```text
Database object
ThreadPool
SegmentReader cache
BufferPool
BlockPool
```

因为上一个进程已经全部销毁了。

---

## SIMD 应拆成两个指标

### SIMD kernel speedup

现有 C++ microbenchmark 负责：

```text
scalar kernel
vs
AVX2 kernel
```

比如：

```text
compare
gather
arithmetic
```

输出：

```text
ns/element
GB/s
speedup
```

---

### SIMD query speedup

用新的：

```text
bench_query_workload
```

在同一进程里：

```text
scalar
vs
AVX2
```

然后：

```text
SIMD query speedup =
median scalar execution latency
-------------------------------
median AVX2 execution latency
```

再输出：

```text
QPS scalar
QPS AVX2
```

---

# 九、SIMD A/B 顺序也必须整改

现在基本都是：

```text
scalar
然后 simd
```

固定顺序。

可能受到：

```text
CPU boost
CPU thermal state
page cache
frequency scaling
```

影响。

采用 paired benchmark：

```text
trial 0: scalar → avx2
trial 1: avx2   → scalar
trial 2: scalar → avx2
trial 3: avx2   → scalar
```

或者固定随机种子：

```cpp
std::mt19937 rng(20260919);
shuffle(order);
```

最终比较 paired median。

---

# 十、三个 microbenchmark 有具体代码问题

### `bench_arith_kernel.cpp`

这里：

```cpp
x = x * 1103515245.0 + 12345.0;
v[i] = (x / 2147483648.0) - 0.5;
```

没有整数 PRNG 中应有的溢出/modulo。

double 会越来越大，几十次迭代之后就可能进入：

```text
inf
```

所以注释：

```text
大约 [-0.5, 0.5)
```

实际上不成立。

直接改为：

```cpp
std::mt19937_64 rng(20260919);
std::uniform_real_distribution<double> dist(-0.5, 0.5);

for (...) {
    v[i] = dist(rng);
}
```

这是必须修。

---

### `bench_compare_kernel.cpp`

现在 timed function：

```cpp
kernel(...);
g_sink += g_mask.Count();
```

所以得到的：

```text
scalar(ns)
avx2(ns)
```

实际是：

```text
compare kernel
+
SelectionMask::Count()
```

count=16/64 这种极短 kernel 时，`Count()` 占比可能很明显。

应改成：

```text
timer start

for N:
    kernel()

timer end

DoNotOptimize(mask)
```

sink 只在整个 sample 后做一次。

---

### `bench_sparse_projection.cpp`

现在：

```cpp
gather()
arith()

for every active row:
    acc += dst[i]
```

这个 scalar accumulation 也在 timed region。

因此所谓：

```text
AVX2 ns/row
```

里面又加入了一次完整的 scalar output scan。

必须拆成：

```text
kernel-only:
gather + arithmetic

pipeline:
gather + arithmetic + downstream consume
```

局部 AVX2 收益用前者。

---

# 十一、`bench_global_aggregate` 也要拆

现在 Global Fast Path 内：

```text
construct HashAggregateState
consume
construct output specs
construct BufferPool
NextResult
read result
```

而 legacy path 不是完全对称的 finalization。

所以现在数字并非纯：

```text
HashAggregate Consume fast path
```

建议拆成：

```text
aggregate_consume_ns/row
aggregate_full_state_ns/row
```

第一项：

```text
只计 Consume()
```

第二项：

```text
state creation
+ consume
+ finalize
```

另外当前：

```cpp
g_sink += r.count;
```

对于：

```text
SUM(value)
```

count 是 0。

sink 必须同时消费：

```cpp
g_sink_count ^= r.count;
g_sink_sum += static_cast<double>(r.sum);
```

否则 SUM benchmark 对编译器优化并不够稳妥。

---

# 十二、所有 microbench 统一一个 harness

新增：

```text
benchmark/benchmark_common.h
benchmark/benchmark_common.cpp
```

不要每个文件自己写一个 `TimeNs()`。

统一：

```cpp
struct BenchmarkStats {
    double min_ns;
    double median_ns;
    double mean_ns;
    double mad_ns;
};

template<class Fn>
BenchmarkStats RunMicroBenchmark(
    Fn&& fn,
    uint32_t warmup_rounds,
    uint32_t measured_rounds,
    uint64_t inner_iterations);
```

默认：

```text
warmup = 3 rounds
samples = 15
```

每个 sample 应运行足够多次，使 sample 本身至少达到大约几十毫秒，而不是去测单个几百纳秒操作。

主要报告：

```text
median
MAD
```

而不是只有一条平均值。

---

# 十三、现有脚本有个更严重的问题：错误被吃掉了

Filter / Projection / SIMD 脚本里存在：

```bash
... || true
```

例如：

```bash
run_sql ... >/dev/null 2>&1 || true
```

这意味着：

```text
查询 crash
SQL 执行失败
benchmark 返回错误
```

仍然会记录一个 latency。

甚至可能因为快速失败，看起来：

> “性能非常好”。

必须全部删除。

统一：

```bash
if ! run_sql ...; then
    echo "benchmark query failed" >&2
    exit 1
fi
```

任何 performance sample 失败：

```text
整个 benchmark invalid
```

不得继续统计。

---

# 十四、正确性检查也需要整改

`bench_execution_modes.sh` 和 `bench_simd.sh` 很多地方只比较：

```text
row count
```

比如两个实现都返回：

```text
1 row
```

但：

```text
SUM = 123
```

和：

```text
SUM = 456
```

row count 还是一样。

因此 correctness 不能只检查：

```text
rows scalar == rows SIMD
```

新增：

```cpp
struct ResultDigest {
    uint64_t row_count;
    uint64_t hash1;
    uint64_t hash2;
};
```

每个输出 row 计算 hash。

对于 unordered parallel output：

```text
hash1 += H(row)
hash2 += H(row) * H(row)
```

最后比较：

```text
row_count
hash1
hash2
```

这样不用保存和 sort 几百万行结果。

另外 `bench_filter_simd.sh --mode multi` 当前直接 diff stdout，但 multi scan 行序本来就可能不同，会产生 false mismatch。

这个也会因此解决。

---

# 十五、Arena / BufferPool 的 P99 目前根本没有可用 benchmark

你之前想证明：

> 使用 Arena 和内存池后 P99 latency 降低。

目前仓库还不能严谨证明。

需要先增加**可切换 baseline**。

## BufferPool

给 `BufferPool` 加：

```cpp
enum class BufferPoolMode {
    POOLED,
    DIRECT
};
```

DIRECT：

```cpp
Acquire()
    → aligned operator new

Release()
    → aligned operator delete
```

完全绕开 freelist。

这样：

```text
DIRECT
vs
POOLED
```

才能形成真正 A/B。

不能简单使用：

```text
max_cached_per_class = 0
```

因为这样仍会经过 freelist mutex，baseline 不干净。

---

## Arena

建议把执行层从：

```cpp
Arena&
```

稍微抽象成：

```cpp
std::pmr::memory_resource*
```

`QueryMemoryContext` 提供：

```cpp
std::pmr::memory_resource*
CoordinatorResource();

std::pmr::memory_resource*
WorkerResource(size_t id);
```

配置：

```cpp
enum class QueryMemoryMode {
    ARENA,
    NEW_DELETE
};
```

ARENA：

```text
Arena
→ BlockPool
```

NEW_DELETE：

```cpp
std::pmr::new_delete_resource()
```

于是可以做真正：

```text
system allocator
vs
Arena
```

而且不会修改 HashAggregate 算法本身。

---

# 十六、P99 allocator benchmark 的固定实验矩阵

选内存分配明显的查询，例如 GROUP BY：

```sql
SELECT id, SUM(value)
FROM bench_data
GROUP BY id;
```

以及 global aggregate：

```sql
SELECT SUM(value)
FROM bench_data
WHERE value >= 0.5;
```

固定：

```text
same binary
same dataset
same CPU
same thread count
same query
same result mode
```

A/B：

```text
A:
QueryMemoryMode = NEW_DELETE
BufferPoolMode  = DIRECT

B:
QueryMemoryMode = ARENA
BufferPoolMode  = POOLED
```

输出：

```text
P50
P95
P99
QPS
system_allocations/query
pool_hit_rate
```

定义：

```text
P99 reduction =
(P99_baseline - P99_pool)
-------------------------
P99_baseline
× 100%
```

简历才能写：

> Arena + BufferPool 将 P99 延迟降低 XX%。

---

# 十七、allocation counters 必须使用 delta

你目前已经有：

```cpp
system_allocations
pool_hits
pool_returns
```

这些是 Database 生命周期累计值。

benchmark 不应该直接打印最终绝对值。

要：

```cpp
before = pool.stats();

run benchmark;

after = pool.stats();
```

计算：

```text
system_allocations =
after.system_allocations
-
before.system_allocations
```

然后：

```text
allocations/query

pool_hit_rate =
pool_hits
-----------------------------
pool_hits + system_allocations
```

这会比一个累计数字有意义得多。

---

# 十八、L3 Cache Miss 也需要单独体系

目前实际上没有实现。

以后至少记录：

```text
cycles
instructions

L1-dcache-loads
L1-dcache-load-misses

LLC-loads
LLC-load-misses

branches
branch-misses
```

主指标不要只写：

```text
LLC misses = xxx
```

要写：

```text
LLC miss rate =
LLC-load-misses / LLC-loads
```

以及：

```text
LLC MPKI =
LLC-load-misses
---------------
instructions
× 1000
```

因为 query 执行时间变化以后，单纯 raw miss 数不好比较。

这对以后验证 Delta encoding 很重要。

---

# 十九、benchmark 环境也必须固定

每次正式出性能结果，都打印：

```text
commit SHA
build type
compiler + version
CPU model
logical CPUs
physical cores
AVX2 availability

execution mode
scan threads
compute threads

vector batch size
queue capacity
dataset rows
segment count
```

正式 benchmark 建议 CPU affinity 固定。

例如单线程 microbench：

```text
固定到同一个 physical core
```

多线程：

```text
固定到指定 core set
```

否则调度迁移本身会制造噪声。

---

# 二十、最终建议重构后的 benchmark 目录

我建议最终整理成：

```text
benchmark/
├── benchmark_common.h
├── benchmark_common.cpp
│
├── bench_query_workload.cpp
│   ├ QPS
│   ├ P50/P95/P99
│   ├ SQL steady latency
│   └ materialized vs blackhole
│
├── bench_parallel_scan.cpp
│   ├ input rows/s
│   ├ output rows/s
│   └ parallel speedup
│
├── bench_arith_kernel.cpp
├── bench_compare_kernel.cpp
├── bench_sparse_projection.cpp
├── bench_selection_pipeline.cpp
├── bench_global_aggregate.cpp
│
├── bench_allocator.cpp
│   ├ system vs Arena
│   ├ direct vs BufferPool
│   └ allocation counters
│
├── bench_perf_counters.cpp
│   ├ cycles
│   ├ instructions
│   ├ LLC miss
│   └ branch miss
│
├── verify_simd.sh
└── run_benchmark_suite.sh
```

shell 脚本只负责：

```text
build
prepare dataset
调用 benchmark executable
汇总结果
```

**shell 不再负责纳秒级计时。**

---

## 整改实施顺序

1. **先实现 `bench_query_workload.cpp`**，彻底替换现在通过反复启动 `simple_olap` 测 latency/QPS 的方式；增加 persistent Database、concurrency、per-query latency samples、QPS、P50/P95/P99。

2. **增加 `Connection::Execute()` blackhole/streaming 路径**，将 engine benchmark 与 `QueryResult` 深拷贝彻底分开，同时保留原 `Query()` API。

3. **整改 `bench_parallel_scan`**：增加 physical scanned rows，主指标改为 `input_rows/s`，speedup 使用 median，`--batch` 改名 `--load-batch`。

4. **统一 microbenchmark harness**，增加 warmup、15+ samples、median/MAD、A/B 交错顺序、统一 benchmark sink。

5. 修复 `bench_arith_kernel` 浮点数据生成；修复 compare 的 `Count()` 污染；修复 sparse projection 的 checksum 污染；拆分 global aggregate consume/finalize。

6. **删除所有性能脚本中的 `|| true`**；correctness 从 row count 升级为 result digest。

7. 增加 `BufferPoolMode::DIRECT/POOLED` 和 `QueryMemoryMode::NEW_DELETE/ARENA`，实现真正的 allocator A/B，再测 P99。

8. 最后加入 hardware counters，正式测 AVX2、Arena、BufferPool、以后 Delta encoding 对 `cycles / IPC / LLC miss / branch miss` 的影响。

完成前 1～6 以后，**QPS、P99、整体 AVX2 提升、并行加速比这四类数字才基本达到可以用于项目报告和简历的可信程度**；完成 7～8 后，你的 “Arena/内存池降低 P99”“优化降低 L3 Cache Miss” 这类性能卖点才有严谨实验依据。

另外有一点需要特别注意：现在仓库里的 `--batch` 是**数据生成/写入 batch**，并不是执行层 1024-row `VectorBatch`。这个地方如果不改名，很容易导致之后错误地宣称“测试了不同 vector batch size”。
