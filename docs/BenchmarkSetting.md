可以。下面这份方案完全按照你当前 `master` 的实际代码制定，基准为最新提交 **`6569fdf`**。目标不是重做已有 benchmark，而是在现有体系上补齐一套能够正式回答 **QPS、AVX2 局部/整体收益、Arena+内存池 P99 收益** 的统一性能测试框架。

## 1. 当前代码基线

当前性能相关调用链实际是：

```text
Connection::Query()
    │
    ├─ QueryMemoryContext
    │    ├─ coordinator Arena
    │    └─ worker Arena × compute_threads
    │
    ├─ Lexer
    ├─ Parser
    ├─ Binder
    ├─ Planner
    ├─ Optimizer
    ├─ PhysicalPlanner
    │
    ├─ ExecutionContext
    │    ├─ ThreadPool
    │    ├─ QueryMemoryContext
    │    └─ BufferPool
    │
    ├─ ExecutionEngine
    │
    └─ QueryResult::Append()
         └─ 深拷贝结果到 BufferPool
```

这意味着以后定义的“端到端查询延迟”应当直接测：

```cpp
Connection::Query(sql)
```

而不是只测 `ExecutionEngine`。这样才真实覆盖 parser → planner → executor → result materialization。

当前代码中还已经具备三个非常好的测试条件：

| 功能                      | 当前实现                                            |
| ----------------------- | ----------------------------------------------- |
| AVX2 A/B                | `SIMPLE_OLAP_FORCE_SCALAR=1`                    |
| Arena backing pool      | `BlockPool`，默认 1 MB block                       |
| VectorBatch memory pool | `BufferPool`，4/8/16/32/64 KB                    |
| BlockPool 开关基础          | `block_pool_max_cached_blocks`                  |
| BufferPool 开关基础         | `buffer_pool_max_cached_per_class`              |
| Pool 统计                 | `system_allocations / pool_hits / pool_returns` |

因此不应该通过旧 commit 和新 commit 比较性能。所有测试应在 **同一个 `6569fdf+` 代码版本、同一个二进制逻辑、同一份数据** 上进行消融。

---

# 2. 最终新增文件

建议新增：

```text
benchmark/
├── bench_query_workload.cpp       # 核心：QPS / E2E latency / P99
├── bench_common.h                 # benchmark 公共结构、统计、数据集
├── run_perf_suite.sh              # 一键跑完整实验矩阵
│
├── bench_compare_kernel.cpp       # 已存在，修改输出
├── bench_arith_kernel.cpp         # 已存在
├── bench_sparse_projection.cpp    # 已存在
├── bench_selection_pipeline.cpp   # 已存在
├── bench_global_aggregate.cpp     # 已存在
│
├── bench_filter_simd.sh           # 保留，主要用于正确性回归
└── bench_projection_simd.sh       # 保留，主要用于正确性回归
```

不建议再分别写 `bench_qps.cpp`、`bench_p99.cpp`、`bench_avx2_e2e.cpp`。它们最终都需要运行 `Connection::Query()`，应该统一由 `bench_query_workload.cpp` 完成，避免不同 benchmark 出现不同计时口径。

---

# 3. `bench_common.h`

新增统一配置结构：

```cpp
struct BenchOptions {
    std::filesystem::path db_path;

    uint64_t rows = 4'000'000;

    size_t clients = 1;

    uint32_t warmup_seconds = 10;
    uint32_t duration_seconds = 60;
    uint64_t min_samples = 0;

    ExecutionMode execution_mode =
        ExecutionMode::SINGLE_THREAD;

    size_t thread_count = 1;
    size_t scan_threads = 1;
    size_t compute_threads = 1;
    size_t queue_capacity = 16;

    QueryMemoryMode memory_mode =
        QueryMemoryMode::ARENA;

    size_t block_pool_cache = 64;
    size_t buffer_pool_cache = 64;

    std::string workload = "mixed";

    bool prepare_data = false;
};
```

统计结构：

```cpp
struct LatencyStats {
    uint64_t query_count = 0;

    double elapsed_seconds = 0;
    double qps = 0;

    double mean_us = 0;

    uint64_t p50_us = 0;
    uint64_t p95_us = 0;
    uint64_t p99_us = 0;
    uint64_t max_us = 0;
};
```

Pool 增量：

```cpp
struct MemoryStats {
    BlockPoolStats block_before;
    BlockPoolStats block_after;

    BufferPoolStats buffer_before;
    BufferPoolStats buffer_after;
};
```

百分位统一使用 nearest-rank：

```cpp
index = ceil(percentile * count) - 1;
```

所有 latency 原始值统一保存为：

```cpp
uint64_t nanoseconds;
```

最终打印时再转换为 μs/ms。

---

# 4. 新增标准 benchmark 数据集

不要继续把现在 CLI 里的：

```text
(id INT64, value DOUBLE)
```

作为完整性能测试数据。

它无法合理制造低/高基数 GROUP BY，因此 Arena 测试会很别扭。

在 `bench_query_workload.cpp` 内部直接通过你现有：

```text
Database
 → CreateTable()
 → StorageManager::GetTable()
 → TableStorage::Append()
```

创建：

```text
perf_data
------------------------------------------------
id          INT64
group_low   INT64
group_high  INT64
value       DOUBLE
value2      DOUBLE
```

数据规则固定：

```cpp
id         = row_id;
group_low  = row_id % 1024;
group_high = row_id % 65536;

value  ~ U(0.0, 1.0);
value2 ~ U(0.0, 1000.0);
```

固定种子：

```cpp
std::mt19937_64 rng(20260919ULL);
```

标准规模：

| profile  |       rows | 用途            |
| -------- | ---------: | ------------- |
| latency  |  1,000,000 | P99、大量重复查询    |
| standard |  4,000,000 | 正式 QPS / AVX2 |
| large    | 10,000,000 | 吞吐与扩展性        |

表**不定义 PRIMARY KEY / KEY**，保证测试主体走 SeqScan，而不会因为你最近新增的 Key Index 被 PhysicalPlanner 改为 IndexScan。

---

# 5. 固定 5 条 workload SQL

SQL 要严格对应你当前实际优化路径。

### Q1：Storage SIMD Compare

```sql
SELECT COUNT(*)
FROM perf_data
WHERE value > 0.5;
```

当前 optimizer 对简单：

```text
column OP constant
```

可下推到 Storage，因此主要覆盖：

```text
Segment
 → StorageRowFilter
 → CompareKernel
 → SelectionMask
 → Global Aggregate
```

这是测 storage AVX2 最合适的端到端 SQL。

### Q2：Execution Filter SIMD

```sql
SELECT COUNT(*)
FROM perf_data
WHERE value >= 0.5;
```

你当前 `bench_filter_simd.sh` 已明确利用：

```text
>= / <= / !=
```

作为 residual Filter，因此这条实际覆盖：

```text
SeqScan
 → FilterOperator
 → VectorPredicate
 → AVX2 CompareKernel
 → Global Aggregate
```

Q1 与 Q2 必须分开，否则无法区分 Storage SIMD 和 execution SIMD。

### Q3：Sparse Projection / Gather

```sql
SELECT id, value + 1.0
FROM perf_data
WHERE value > 0.999;
```

约 0.1% selection。

对应：

```text
Storage Filter
 → SelectionMask
 → Projection
 → typed gather
 → dense arithmetic SIMD
 → QueryResult::Append
```

同时可以测：

```text
AVX2 gather/arithmetic
BufferPool
结果物化
```

因为只返回约千分之一的数据，不会让 `QueryResult::Append()` 完全淹没查询本身。

### Q4：低基数 Hash Aggregate

```sql
SELECT group_low, COUNT(*), SUM(value)
FROM perf_data
WHERE value > 0.2
GROUP BY group_low;
```

1024 groups。

主要测试正常 OLAP：

```text
Scan
 → Filter
 → HashAggregateState
 → PMR/Arena
```

### Q5：高基数 Hash Aggregate

```sql
SELECT group_high, COUNT(*), SUM(value)
FROM perf_data
GROUP BY group_high;
```

65536 groups。

这是 **Arena / BlockPool P99 主测试**。

这里会大量产生：

```text
GroupKey
pmr::unordered_map node
StateVector
group hash state
```

与你当前 Arena 实际服务的对象完全对应。

---

# 6. Mixed workload

统一定义：

| Query |  比例 |
| ----- | --: |
| Q1    | 25% |
| Q2    | 25% |
| Q3    | 15% |
| Q4    | 20% |
| Q5    | 15% |

不要使用随机分布实时选择。

直接建立固定长度 100 的：

```cpp
std::array<QueryId, 100> schedule;
```

每个 client 循环执行。

这样每个测试配置的 workload 比例完全确定，避免 RNG 本身和随机波动。

---

# 7. `bench_query_workload.cpp` 执行结构

程序生命周期必须是：

```text
process start
    │
    ├─ 构造 DatabaseConfig
    ├─ Database db
    │
    ├─ 打开 perf_data
    ├─ 预热
    │
    ├─ snapshot pool stats
    │
    ├─ 正式 measurement
    │
    ├─ snapshot pool stats
    │
    ├─ 计算 QPS/P50/P95/P99
    │
    └─ 输出 CSV
process exit
```

不能像现在一些 `.sh` benchmark 那样：

```text
每条 SQL
→ 启动 simple_olap
→ 查询
→ exit
```

---

# 8. 多 client 实现

每个 client 单独持有：

```cpp
Connection connection(database);
```

线程模型：

```text
              Database
          /      |       \
         /       |        \
Connection0 Connection1 Connection2 ...
     |          |           |
 client 0    client 1     client 2
```

`Connection` 当前只有：

```cpp
Database& database_;
```

不存在 Connection 自身共享可变查询状态，因此适合作为 client-local 对象。

但开始 client 线程之前必须：

```cpp
auto table_id = db.GetCatalog().FindTable("perf_data");
auto* entry = db.GetCatalog().GetTable(*table_id);

db.GetStorageManager().GetTable(
    *table_id,
    entry->schema);
```

先将表打开一次。

这是因为当前：

```cpp
StorageManager::tables_
```

是普通：

```cpp
std::unordered_map
```

没有 mutex。

正式测量阶段应该保证它只发生并发 `find/read`，不能让多个 client 同时第一次触发 lazy open。

同样：

```cpp
Database::SetExecutionMode()
```

只能在启动 client 之前设置。

measurement 内不得修改 DatabaseConfig。

---

# 9. QPS 定义

每个线程：

```cpp
while (now < deadline) {
    const auto begin = Clock::now();

    QueryResult result = connection.Query(sql);

    const auto end = Clock::now();

    latency.push_back(end - begin);

    ++completed;
}
```

QPS：

```text
total completed queries
-----------------------
actual elapsed seconds
```

不是：

```text
1 / average latency
```

并发环境下二者不是一回事。

---

# 10. 结果销毁位置

这里要保持明确的用户可见延迟语义：

```cpp
auto begin = Clock::now();

QueryResult result = connection.Query(sql);

auto end = Clock::now();        // latency 截止这里

Record(end - begin);

ConsumeDigest(result);

result = {};                    // 下一 query 前释放
```

因此 P99 定义为：

> `Connection::Query()` 从调用到返回 `QueryResult` 的端到端延迟。

不会把调用方后续持有结果的时间算入数据库 latency。

但是每次下一条 query 前保证旧结果已经析构，使 BufferPool 能正常复用。

---

# 11. QPS 正式测试矩阵

先测 **inter-query concurrency**。

强制：

```text
ExecutionMode::SINGLE_THREAD
```

否则：

```text
4 clients × 8 compute worker
```

实际上是在制造 32 个执行 worker，无法解释 QPS。

正式：

| clients | execution | scan | compute |
| ------: | --------- | ---: | ------: |
|       1 | SINGLE    |    1 |       1 |
|       2 | SINGLE    |    1 |       1 |
|       4 | SINGLE    |    1 |       1 |
|       8 | SINGLE    |    1 |       1 |

测试：

```text
Q1
Q2
Q4
mixed
```

每组：

```text
warmup       = 10 s
measurement  = 60 s
repeat       = 5
```

最终正式结果使用：

```text
5 个 round 的 QPS median
5 个 round 的 P99 median
```

而不是 best。

---

# 12. 单 Query 内部并行另测

当前项目已经有：

```text
ExecutionMode::MULTI_THREAD
ParallelExecutor
ParallelScanSession
worker-local HashAggregateState
```

所以并行收益必须单独测：

```text
clients = 1
```

配置：

| compute/scan threads | clients |
| -------------------: | ------: |
|                    1 |       1 |
|                    2 |       1 |
|                    4 |       1 |
|                    8 |       1 |

主要跑：

```text
Q1
Q4
Q5
```

输出：

```text
QPS
P50
P99
parallel speedup
```

不要把这个数字混入普通 QPS。

---

# 13. AVX2 整体性能测试

你当前：

```cpp
CpuFeatures::Detect()
```

已经读取：

```text
SIMPLE_OLAP_FORCE_SCALAR
```

并且 `KernelRegistry` 初始化一次后固定 backend。

因此不需要增加任何 SIMD 开关。

同一个：

```text
bench_query_workload
```

运行两个**独立进程**。

Scalar：

```bash
SIMPLE_OLAP_FORCE_SCALAR=1 \
./build/bin/bench_query_workload ...
```

AVX2：

```bash
./build/bin/bench_query_workload ...
```

必须满足：

```text
相同 executable
相同 DB 文件
相同 query schedule
相同 clients
相同 execution mode
相同 threads
相同 Pool 配置
```

AVX2 正式 E2E 测：

| workload | 作用                     |
| -------- | ---------------------- |
| Q1       | Storage compare        |
| Q2       | FilterOperator compare |
| Q3       | gather + arithmetic    |
| Q4       | SIMD + hash aggregate  |
| mixed    | 数据库整体收益                |

整体提升：

```text
QPS improvement =
(QPS_AVX2 - QPS_scalar)
-----------------------
      QPS_scalar
```

同时记录：

```text
P50 scalar / AVX2
P95 scalar / AVX2
P99 scalar / AVX2
```

---

# 14. AVX2 局部性能测试

这里不需要重新实现。

直接利用：

```text
bench_compare_kernel
bench_arith_kernel
bench_sparse_projection
bench_selection_pipeline
```

但 `bench_compare_kernel.cpp` 当前有一个需要修正的指标名称。

目前：

```cpp
speedup = old_ns / avx2_ns;
```

所以当前显示的 `speedup` 实际是：

```text
old AVX2 / new AVX2
```

而不是 scalar / AVX2。

修改成同时输出：

```cpp
const double scalar_speedup =
    scalar_ns / avx2_ns;

const double optimization_speedup =
    old_ns / avx2_ns;
```

最终：

| type | scalar ns | old AVX2 | new AVX2 | scalar→AVX2 | old→new |
| ---- | --------: | -------: | -------: | ----------: | ------: |

这样能够分别证明：

```text
SIMD 本身收益
```

和：

```text
你新加入 64-row bitmap block 的增量收益
```

不能混为一个数字。

---

# 15. Arena 消融需要修改的核心接口

这是唯一需要对现有执行代码做结构性小修改的部分。

当前：

```cpp
class QueryMemoryContext {
public:
    Arena& CoordinatorArena();
    Arena& WorkerArena(size_t);
};
```

而实际上 `HashAggregateState` 已经接受：

```cpp
std::pmr::memory_resource*
```

因此现在把 Arena 类型暴露出去反而限制了 benchmark。

新增：

```cpp
enum class QueryMemoryMode : uint8_t {
    ARENA = 0,
    SYSTEM = 1,
};
```

修改 `QueryMemoryContext`：

```cpp
class QueryMemoryContext {
public:
    QueryMemoryContext(
        BlockPool& block_pool,
        size_t worker_count,
        QueryMemoryMode mode =
            QueryMemoryMode::ARENA);

    std::pmr::memory_resource*
    CoordinatorResource() noexcept;

    std::pmr::memory_resource*
    WorkerResource(size_t worker_id);

private:
    QueryMemoryMode mode_;

    std::unique_ptr<Arena> coordinator_arena_;

    std::vector<
        std::unique_ptr<Arena>
    > worker_arenas_;
};
```

ARENA：

```cpp
CoordinatorResource()
    -> coordinator_arena_.get();

WorkerResource(i)
    -> worker_arenas_[i].get();
```

SYSTEM：

```cpp
std::pmr::new_delete_resource()
```

---

# 16. 实际调用点只需改三处

当前 `ExecutorBuilder::BuildHashAggregate()`：

```cpp
&ctx_->memory->CoordinatorArena()
```

修改：

```cpp
ctx_->memory->CoordinatorResource()
```

当前 `ParallelExecutor` worker：

```cpp
Arena& arena =
    ctx_->memory->WorkerArena(worker_id);

HashAggregateState local(
    ...,
    &arena);
```

修改：

```cpp
auto* resource =
    ctx_->memory->WorkerResource(worker_id);

HashAggregateState local(
    ...,
    resource);
```

global state：

```cpp
Arena& coordinator_arena =
    ctx_->memory->CoordinatorArena();
```

改为：

```cpp
auto* coordinator_resource =
    ctx_->memory->CoordinatorResource();
```

`HashAggregateState`、`HashAggregateOperator` 本身**无需修改**，因为它们已经正确使用：

```cpp
std::pmr::memory_resource*
```

这正是当前代码非常适合做 Arena A/B 测试的地方。

---

# 17. `DatabaseConfig` 增加一个字段

当前：

```cpp
struct DatabaseConfig {
    size_t arena_block_size;
    size_t block_pool_max_cached_blocks;
    size_t buffer_pool_max_cached_per_class;
    ...
};
```

增加：

```cpp
QueryMemoryMode query_memory_mode =
    QueryMemoryMode::ARENA;
```

`Connection::Query()`：

当前：

```cpp
QueryMemoryContext query_memory(
    database_.GetBlockPool(),
    worker_count);
```

修改：

```cpp
QueryMemoryContext query_memory(
    database_.GetBlockPool(),
    worker_count,
    database_.GetConfig().query_memory_mode);
```

默认仍然是：

```text
ARENA
```

因此生产行为完全不变。

---

# 18. BlockPool / BufferPool 不需要新增 enable 开关

当前代码已经天然支持关闭 cache。

BlockPool：

```cpp
if (free_blocks_.size()
    < max_cached_blocks_) {
    ...
}
```

因此：

```cpp
block_pool_max_cached_blocks = 0;
```

等价于：

```text
Arena block 不跨 query 缓存
Release → ::operator delete
```

BufferPool 同理：

```cpp
buffer_pool_max_cached_per_class = 0;
```

即可让：

```text
Acquire → ::operator new
Release → ::operator delete
```

因此不要再增加：

```cpp
bool enable_buffer_pool;
bool enable_block_pool;
```

会形成两套逻辑，没必要。

---

# 19. 内存优化正式消融矩阵

定义四种模式：

| Mode | PMR                   | BlockPool cache | BufferPool cache |
| ---- | --------------------- | --------------: | ---------------: |
| M0   | `new_delete_resource` |               0 |                0 |
| M1   | Arena                 |               0 |                0 |
| M2   | Arena                 |              64 |                0 |
| M3   | Arena                 |              64 |               64 |

这样严格得到：

```text
M0 → M1
Arena batching 收益

M1 → M2
BlockPool 跨 Query 复用收益

M2 → M3
VectorBatch BufferPool 收益

M0 → M3
完整 memory subsystem 收益
```

这是比“memory commit 前后比较”严谨得多的实验。

---

# 20. P99 测试 workload

Arena 主测试：

```text
Q5
```

即：

```sql
SELECT group_high, COUNT(*), SUM(value)
FROM perf_data
GROUP BY group_high;
```

BufferPool 主测试：

```text
Q3
```

即：

```sql
SELECT id, value + 1.0
FROM perf_data
WHERE value > 0.999;
```

整体：

```text
mixed
```

正式测：

| profile       | clients | execution |
| ------------- | ------: | --------- |
| single-client |       1 | SINGLE    |
| concurrent    |       4 | SINGLE    |

数据使用：

```text
1M rows
```

P99 测试同时要求：

```text
measurement >= 60 秒
AND
sample_count >= 5000
```

即满足两个条件后才结束。

若某个 workload 很重，最低不能少于：

```text
2000 samples
```

否则 P99 尾部样本不足。

---

# 21. Pool Stats 测量边界

warmup 完成后：

```cpp
auto block_before =
    db.GetBlockPool().stats();

auto buffer_before =
    db.GetBufferPool().stats();
```

正式 measurement 后：

```cpp
auto block_after =
    db.GetBlockPool().stats();

auto buffer_after =
    db.GetBufferPool().stats();
```

输出差值：

```text
block_system_allocations
block_pool_hits
block_pool_returns

buffer_system_allocations
buffer_pool_hits
buffer_pool_returns
```

不能直接输出程序启动以来的绝对值，因为 warmup 会污染统计。

---

# 22. 建议额外增加 system free 统计

当前两个 Pool 只有：

```cpp
system_allocations
pool_hits
pool_returns
```

建议补：

```cpp
uint64_t system_deallocations;
```

BlockPool 在：

```cpp
FreeBlock()
```

中递增。

BufferPool 在所有：

```cpp
::operator delete(...)
```

路径递增。

对应 stats：

```cpp
struct BlockPoolStats {
    uint64_t system_allocations;
    uint64_t system_deallocations;
    uint64_t pool_hits;
    uint64_t pool_returns;
    size_t cached_blocks;
};
```

`BufferPoolStats` 同样增加。

这样内存优化测试可以真正证明：

```text
new/delete 次数下降
          ↓
尾延迟下降
```

而不是只观察 P99。

---

# 23. `run_perf_suite.sh`

最终所有实验统一由这个脚本调度。

唯一建议的执行阶段如下：

1. Release 构建；生成一次 `perf_data`；运行 correctness smoke test；运行 QPS clients=1/2/4/8；分别运行 scalar / AVX2 的 Q1/Q2/Q3/Q4/mixed；运行现有四个 SIMD micro benchmark；运行 M0/M1/M2/M3 的 Q3/Q5/mixed P99；最后写出统一 CSV。

这里要特别注意：

```text
SIMPLE_OLAP_FORCE_SCALAR
```

只能通过**独立进程**测试。

不能在：

```cpp
bench_query_workload
```

内部：

```cpp
setenv()
→ scalar
setenv()
→ avx2
```

因为当前 `KernelRegistry` 是一次初始化的。

---

# 24. CMake 修改

当前 benchmark block 中增加：

```cmake
add_executable(
    bench_query_workload
    ${CMAKE_SOURCE_DIR}/benchmark/bench_query_workload.cpp)

target_link_libraries(
    bench_query_workload
    PRIVATE simple_olap_core)

target_compile_definitions(
    bench_query_workload PRIVATE
    SIMPLE_OLAP_BENCH_ROOT_DIR="${CMAKE_SOURCE_DIR}")
```

不改变：

```text
simple_olap_core
```

的编译选项。

AVX2 仍然保持当前正确设计：

```text
普通源文件：-O3
AVX2 source：-O3 -mavx2
runtime dispatch
```

禁止为了 benchmark 给整个 target 加：

```text
-march=native
-mavx2
```

否则 scalar 对照组失去意义。

---

# 25. CSV 输出格式

每一组 benchmark 输出一行：

```text
benchmark
backend
workload
rows
clients
execution_mode
scan_threads
compute_threads
memory_mode
block_cache
buffer_cache
elapsed_s
queries
qps
mean_us
p50_us
p95_us
p99_us
max_us
block_system_alloc
block_system_free
block_hits
block_returns
buffer_system_alloc
buffer_system_free
buffer_hits
buffer_returns
```

例如：

```text
query_workload,
avx2,
mixed,
4000000,
4,
single,
1,
1,
arena,
64,
64,
60.002,
15342,
255.69,
...
```

以后 README 的所有性能数字必须来自这份 CSV，而不是人工摘取不同 benchmark 输出。

---

# 26. 最终性能报告只保留四类结果

### QPS

| clients | QPS | P50 | P95 | P99 |
| ------: | --: | --: | --: | --: |
|       1 |     |     |     |     |
|       2 |     |     |     |     |
|       4 |     |     |     |     |
|       8 |     |     |     |     |

### AVX2 local

| Path           | Scalar | AVX2 | speedup |
| -------------- | -----: | ---: | ------: |
| Compare INT32  |        |      |         |
| Compare INT64  |        |      |         |
| Compare DOUBLE |        |      |         |
| Arithmetic     |        |      |         |
| Sparse gather  |        |      |         |

### AVX2 end-to-end

| Query             | scalar QPS | AVX2 QPS | improvement |
| ----------------- | ---------: | -------: | ----------: |
| Storage filter    |            |          |             |
| Execution filter  |            |          |             |
| Sparse projection |            |          |             |
| GROUP BY          |            |          |             |
| Mixed             |            |          |             |

### Memory P99

| Mode           | alloc | free | pool hits | QPS | P50 | P99 |
| -------------- | ----: | ---: | --------: | --: | --: | --: |
| M0 System      |       |      |           |     |     |     |
| M1 Arena       |       |      |           |     |     |     |
| M2 +BlockPool  |       |      |           |     |     |     |
| M3 +BufferPool |       |      |           |     |     |     |

---

## 最终修改范围

| 文件                                                            | 修改                                             |
| ------------------------------------------------------------- | ---------------------------------------------- |
| `src/memory/query_memory_context/query_memory_context.h/.cpp` | 增加 `QueryMemoryMode` 和 Resource 接口             |
| `src/main/execution_context.h`                                | 增加 `query_memory_mode`                         |
| `src/main/connection.cpp`                                     | QueryMemoryContext 注入 mode                     |
| `src/execution/executor_builder.cpp`                          | `CoordinatorArena()` → `CoordinatorResource()` |
| `src/execution/parallel/parallel_executor.cpp`                | worker/global 改用 `memory_resource*`            |
| `src/memory/block_pool/*`                                     | 建议增加 system deallocation 统计                    |
| `src/memory/buffer_pool/*`                                    | 建议增加 system deallocation 统计                    |
| `benchmark/bench_compare_kernel.cpp`                          | 分离 scalar speedup 与 old→new speedup            |
| `benchmark/bench_common.h`                                    | 新增                                             |
| `benchmark/bench_query_workload.cpp`                          | 新增                                             |
| `benchmark/run_perf_suite.sh`                                 | 新增                                             |
| `CMakeLists.txt`                                              | 注册新 benchmark                                  |

这套方案最关键的是：**完全不需要破坏你现在的执行架构**。AVX2 利用现有 runtime dispatch；BufferPool/BlockPool 利用现有 cache=0 做消融；Arena 只需要把目前已经存在的 `std::pmr::memory_resource` 抽象真正暴露出来。最终得到的 QPS、AVX2 整体/局部提升、Arena+Pool P99 降低三个数字，才可以作为 `simple_olap` 的正式性能指标。
