结论很明确：**这次应该把现在的“Scan Stage → BoundedBlockingQueue → Compute Stage”整个拆掉，恢复到你最早那个方向，但要用 query-local 的 Global/Local Scan State 重写，而不是恢复旧的 `TableStorage::SegmentSource` 成员。**

这也正好对应 DuckDB 的核心思路：morsel-driven parallelism，下游 source 将工作单元分给线程，每个线程拿到自己的 local state 后沿整条 pipeline 执行；对于 hash aggregation，每个线程维护自己的局部聚合状态，之后 Combine/Finalize。DuckDB 官方目前仍然采用这种 thread-local pre-aggregation 模式。([DuckDB][1])

---

# 一、当前架构真正的问题

你现在的 aggregate 并行路径实际上是：

```text
                         ParallelScanSession
                               │
              ┌────────────────┴────────────────┐
              │                                 │
         scan worker 0                     scan worker N
              │                                 │
         ScanSegment()                     ScanSegment()
              │                                 │
              └──── BoundedBlockingQueue ───────┘
                               │
                     BatchStream::Next()
                               │
              ┌────────────────┴────────────────┐
              │                                 │
       compute worker 0                  compute worker N
              │                                 │
          SeqScan                           SeqScan
              │                                 │
           Filter                            Filter
              │                                 │
         Projection                        Projection
              │                                 │
     local HashAggregateState       local HashAggregateState
              └────────────────┬────────────────┘
                               │
                         coordinator
                               │
                             Merge
                               │
                           Finalize
```

这里有几个明显问题。

`src/storage/scan/parallel_scan_session.*` 自己创建了：

```cpp
ThreadPool scan_pool_;

BoundedBlockingQueue<VectorBatch> queue_;

std::atomic<size_t> next_segment_;
```

然后 `src/execution/parallel/parallel_executor.cpp` 又用数据库级：

```cpp
ThreadPool* compute_pool_;
```

再跑第二组 worker。

也就是说，你把本来应该属于**同一个 pipeline worker 的 Scan 和 Aggregate**强行切成了两组线程。

于是每个 1024-row `VectorBatch` 都至少经过：

```text
scan
 ↓
move batch
 ↓
queue mutex
 ↓
queue condition_variable
 ↓
Push
 ↓
queue mutex
 ↓
condition_variable
 ↓
Pop
 ↓
compute
```

而且你现在 `ParallelExecutor::ExecuteAggregate()` 已经具备：

```cpp
HashAggregateState local(...)
local.Consume(batch)
...
global.Merge(...)
```

所以实际上 **partial aggregate 这一半已经是正确的**。

错的是：

> batch 到达 `local.Consume()` 之前，被 `ParallelScanSession + BatchStream + BoundedBlockingQueue` 强行绕了一圈。

这就是这次重构应该精确打掉的东西。

---

# 二、最终目标架构

改成：

```text
                     Query ParallelScanGlobalState
                               │
                       atomic next_segment
                               │
              ┌────────────────┼────────────────┐
              │                │                │
          worker 0         worker 1         worker N
              │                │                │
         claim segment    claim segment    claim segment
              │                │                │
          ScanSegment      ScanSegment      ScanSegment
              │                │                │
            Filter           Filter           Filter
              │                │                │
          Projection       Projection       Projection
              │                │                │
       local aggregate   local aggregate   local aggregate
              │                │                │
       claim next seg    claim next seg    claim next seg
              │                │                │
             ...              ...              ...
              └────────────────┼────────────────┘
                               │
                         worker futures
                               │
                         coordinator
                               │
                             Merge
                               │
                           Finalize
                               │
                            Result
```

**Scan → Filter → Project → Partial Aggregate 全部由同一个 worker 连续执行。**

整个 aggregate 热路径中：

```text
没有 BatchStream
没有 BoundedBlockingQueue
没有 scan thread pool
没有 batch Push/Pop
没有 per-batch condition_variable
```

只剩：

```text
每个 segment 一次 atomic fetch_add
+
worker-local pipeline
+
最终 partial aggregate merge
```

这才是你这个项目当前复杂度下应该采用的 DuckDB 风格。

DuckDB 本身也是把可并行扫描工作按存储层工作单元拆分；其文档明确指出扫描并行度受到 row group 数量约束。你的 `Segment` 最大 65,536 行，因此直接把一个 Segment 当作当前版本的 morsel 很合适。([DuckDB][2])

---

# 三、核心改造：加入 Query-Local Parallel Scan State

不要恢复以前这个设计：

```cpp
class TableStorage {
    SegmentSource segmentallocator_;
};
```

你之前那个方向是对的，但**位置不对**。

`next_segment` 绝不能成为 `TableStorage` 的永久状态，否则：

```text
Query A
Query B
```

同时扫描同一张表时会争抢同一个 allocator。

你后来实际上已经意识到了这一点，现在 `ScanCursor` 的注释也明确写着：

> 扫描进度完全由游标自身持有，同一张表可以被多个查询并发扫描而互不干扰。

所以新建：

```text
src/storage/scan/parallel_scan_state.h
```

定义：

```cpp
struct ParallelScanGlobalState {
    std::vector<SegmentId> segment_ids;

    std::atomic<size_t> next_segment{0};

    std::shared_ptr<const PreparedScanPredicates> prepared_predicates;

    std::atomic<bool> cancelled{false};

    size_t MaxThreads() const noexcept;

    std::optional<SegmentId> ClaimSegment() noexcept;

    void Cancel() noexcept;
};
```

语义：

```cpp
ClaimSegment()
{
    const size_t index =
        next_segment.fetch_add(1, std::memory_order_relaxed);

    if (index >= segment_ids.size())
        return std::nullopt;

    return segment_ids[index];
}
```

注意这里 atomic 操作发生频率变成：

```text
原来：
每 1024 rows 发生 Queue Push/Pop + mutex/CV

现在：
每 65536 rows 才发生一次 fetch_add
```

数量级已经完全不同。

---

# 四、再加入 Worker-Local Scan State

同文件定义：

```cpp
struct ParallelScanLocalState {
    SegmentId segment_id = kInvalidSegmentId;

    SegmentReader* reader = nullptr;

    SegmentScanCursor cursor;

    bool has_segment = false;

    void ResetSegment() noexcept;
};
```

这里有一个非常重要的成员：

```cpp
SegmentReader* reader;
```

这是我检查你当前代码后认为**必须顺手修掉**的一点。

你现在：

```cpp
TableStorage::ScanSegment(...)
{
    SegmentReader* reader = GetSegmentReader(segment_id);
```

而：

```cpp
GetSegmentReader(...)
{
    std::lock_guard<std::mutex> lock(reader_cache_mutex_);
```

也就是说，目前一个 Segment：

```text
65536 rows
/
1024 rows per batch
=
64 batches
```

可能为了同一个 `SegmentReader` 调：

```text
64 次 GetSegmentReader()
64 次 reader_cache_mutex_
```

新模型下应该变成：

```text
worker claim segment
        │
        ↓
GetSegmentReader(segment)
        │
   mutex 一次
        │
        ↓
local.reader = reader
        │
        ├─ batch 0
        ├─ batch 1
        ├─ ...
        └─ batch 63
```

即：

> `reader_cache_mutex_` 从 per-batch 锁下降到 per-segment 锁。

这和去 Queue 是同一级别值得做的优化。

---

# 五、TableStorage 新接口

修改：

```text
src/storage/table/table_storage.h
src/storage/table/table_storage.cpp
```

删除并行入口：

```cpp
std::shared_ptr<BatchStream>
CreateParallelScan(
    const ScanOptions& options,
    size_t scan_threads,
    size_t queue_capacity);
```

替换为：

```cpp
std::unique_ptr<ParallelScanGlobalState>
CreateParallelScanState(const ScanOptions& options);
```

以及：

```cpp
bool ScanParallel(
    const ScanOptions& options,
    ParallelScanGlobalState& global_state,
    ParallelScanLocalState& local_state,
    VectorBatch& output);
```

内部再增加一个不需要反复查 reader 的实现：

```cpp
bool ScanSegment(
    SegmentReader& reader,
    const ScanOptions& options,
    SegmentScanCursor& cursor,
    VectorBatch& output);
```

现有：

```cpp
bool ScanSegment(
    SegmentId segment_id,
    ...);
```

可以保留成包装：

```cpp
reader = GetSegmentReader(segment_id);

return ScanSegment(
    *reader,
    options,
    cursor,
    output);
```

这样串行逻辑暂时不需要大改。

---

# 六、`ScanParallel()` 的确定逻辑

这是整个设计最核心的循环：

```cpp
bool TableStorage::ScanParallel(...)
{
    while (true) {

        if (global_state.cancelled)
            return false;

        if (!local_state.has_segment) {

            auto id = global_state.ClaimSegment();

            if (!id)
                return false;

            SegmentReader* reader =
                GetSegmentReader(*id);

            if (!reader)
                continue;

            local_state.segment_id = *id;
            local_state.reader = reader;
            local_state.cursor = SegmentScanCursor{};

            local_state.cursor.prepared_predicates =
                global_state.prepared_predicates.get();

            local_state.has_segment = true;
        }

        bool got = ScanSegment(
            *local_state.reader,
            options,
            local_state.cursor,
            output);

        if (!got) {
            local_state.ResetSegment();
            continue;
        }

        if (output.size == 0)
            continue;

        return true;
    }
}
```

因此一个 worker 的行为就是：

```text
Claim Segment 7

Segment 7:
    batch 0 -> pipeline
    batch 1 -> pipeline
    batch 2 -> pipeline
    ...
    batch 63 -> pipeline

Claim Segment 12

Segment 12:
    ...
```

这就是你的 morsel-driven scan。

---

# 七、重构 `SeqScanOperator`

当前：

```cpp
SeqScanOperator
```

有两种模式：

```text
serial:
    table_->Scan()

parallel:
    stream_->Next()
```

把：

```cpp
std::shared_ptr<BatchStream> stream_;
```

彻底删掉。

新增：

```cpp
ParallelScanGlobalState* parallel_state_ = nullptr;

ParallelScanLocalState local_state_;
```

构造函数改成：

```cpp
// serial
SeqScanOperator(
    TableId table_id,
    ScanOptions options,
    ExecutionContext* ctx);

// parallel worker
SeqScanOperator(
    TableId table_id,
    ScanOptions options,
    ParallelScanGlobalState* parallel_state,
    ExecutionContext* ctx);
```

`Init()` 两种模式都正常获取：

```cpp
table_ = storage.get();
```

不要像现在一样：

```cpp
if (stream_)
    return;
```

`Next()`：

```cpp
if (parallel_state_) {
    return table_->ScanParallel(
        options_,
        *parallel_state_,
        local_state_,
        batch);
}

return table_->Scan(
    options_,
    cursor_,
    batch);
```

因此从这一层开始：

```text
SeqScan::Next()
      ↓
TableStorage::ScanParallel()
      ↓
真正读 Segment
```

没有任何中间数据结构。

---

# 八、ExecutorBuilder 改造

当前：

```cpp
struct ExecutorBuildOptions {
    std::shared_ptr<BatchStream> scan_stream;
};
```

替换成：

```cpp
struct ExecutorBuildOptions {
    ParallelScanGlobalState* parallel_scan = nullptr;
};
```

然后：

```cpp
BuiltExecutor ExecutorBuilder::BuildSeqScan(...)
```

改成：

```cpp
if (options.parallel_scan != nullptr) {
    return BuiltExecutor{
        std::make_unique<SeqScanOperator>(
            spec.table_id,
            std::move(spec.options),
            options.parallel_scan,
            ctx_),
        std::move(spec.output_schema)
    };
}
```

这样已有：

```text
FilterOperator
ProjectionOperator
```

全部不用修改。

这点很重要。

因为每个 worker 都：

```cpp
builder.Build(child_plan, build_options);
```

所以每一个 worker 天然都会得到：

```text
独立 SeqScanOperator
独立 ParallelScanLocalState
独立 FilterOperator
独立 ProjectionOperator
```

但是共享：

```text
ParallelScanGlobalState
```

这和 DuckDB 的：

```text
GlobalSourceState
+
LocalSourceState per thread
```

模型非常接近。当前 DuckDB 的 `PhysicalTableScan` 源码也明确区分 `GetGlobalSourceState()` 和 `GetLocalSourceState()`。

---

# 九、`ExecuteAggregate()` 重写

你现在前 1～3 步基本可以保留：

```cpp
child_plan
FindSeqScanPlan()
BuildScanSpec()
GetTable()
```

接下来删掉：

```cpp
storage->CreateParallelScan(...)

ParallelScanSession::Start()

std::shared_ptr<BatchStream> stream
```

替换成：

```cpp
auto scan_state =
    storage->CreateParallelScanState(
        scan_spec.options);
```

worker 数量：

```cpp
size_t worker_count =
    std::min({
        config_.worker_threads,
        compute_pool_->thread_count(),
        scan_state->MaxThreads()
    });
```

这里：

```cpp
MaxThreads()
=
segment_ids.size();
```

因此：

```text
1 segment  -> 最多 1 worker
2 segments -> 最多 2 workers
8 segments -> 最多 8 workers
```

不会出现：

```text
开 8 个线程
实际上只有 1 个 segment
```

这种纯调度开销。

---

每个 worker：

```cpp
ExecutorBuildOptions options{
    scan_state.get()
};

future = compute_pool_->Submit(
    [&, worker_id]() -> HashAggregateState {

        BuiltExecutor pipeline =
            builder.Build(
                child_plan,
                options);

        pipeline.root->Init();

        Arena& arena =
            ctx_->memory->WorkerArena(worker_id);

        HashAggregateState local(
            &spec.group_exprs,
            &spec.agg_calls,
            &arena);

        local.set_outputs(
            &spec.outputs);

        VectorBatch batch(
            ctx_->buffer_pool);

        while (
            pipeline.root->Next(batch)
        ) {
            if (
                ActiveRowCount(batch) > 0
            ) {
                local.Consume(batch);
            }

            batch.Reset();
        }

        return local;
    });
```

最终就是：

```text
worker
 │
 ├ SeqScan
 │
 ├ Filter
 │
 ├ Projection
 │
 └ HashAggregateState::Consume
```

真正一条路走到底。

---

# 十、异常和取消机制

现在错误依赖：

```cpp
queue_.Abort()
queue_.Cancel()
```

新设计不要再创造另一套 CV。

直接在：

```cpp
ParallelScanGlobalState
```

放：

```cpp
std::atomic<bool> cancelled{false};
```

任意 worker 报错：

```cpp
scan_state->Cancel();
```

其他 worker 在：

```cpp
ClaimSegment()
```

或者下一次：

```cpp
ScanParallel()
```

看到：

```cpp
cancelled == true
```

就退出。

`ParallelExecutor` 仍保持你现在这个正确原则：

```text
即使一个 worker 出错
也必须把所有 future get 完
才能让 ExecuteAggregate 返回
```

因为：

```text
plan
spec
scan_state
Arena
builder
```

都属于 `ExecuteAggregate()` / Query 生命周期。

这一点不要改。

---

# 十一、HashAggregateState 基本不用重写

你当前这部分其实已经非常适合这个架构：

```cpp
HashAggregateState local(...)
```

worker-local。

然后：

```cpp
HashAggregateState global(...);

global.Merge(future.get());
```

coordinator merge。

尤其你刚刚加入的：

```cpp
AggregateMode::GLOBAL
```

非常适合这种模型。

例如：

```sql
SELECT COUNT(*), SUM(value)
FROM t;
```

变成：

```text
worker 0
65K rows
       ↓
[count,sum]

worker 1
65K rows
       ↓
[count,sum]

worker 2
65K rows
       ↓
[count,sum]

          ↓

Coordinator

count0 + count1 + count2
sum0   + sum1   + sum2
```

中间数据可能从几十万个 input rows 直接收缩到每 worker 几十字节。

这正是 thread-local pre-aggregation 的意义。DuckDB 对低基数聚合也是这个方向：线程本地预聚合，然后组合线程局部状态。([DuckDB][1])

---

# 十二、Grouped Aggregate 暂时不要照搬 DuckDB 的全部复杂度

当前：

```cpp
global.Merge(worker_state)
```

还是 coordinator 单线程 merge。

对于：

```sql
SELECT g, COUNT(*)
FROM t
GROUP BY g;
```

如果只有：

```text
10
100
1000 groups
```

这完全够用。

但是如果：

```text
GROUP BY primary_key
```

达到：

```text
1,000,000 groups
```

coordinator merge 最终会成为新瓶颈。

DuckDB 对高基数 grouped aggregation 会进一步做 radix partition，把 thread-local aggregation 后的数据按 partition 分开，再并行 combine。([DuckDB][3])

但我不建议你现在同时实现。

你的第一版应该严格限定为：

```text
Phase 1

parallel scan
    ↓
parallel filter/project
    ↓
thread-local partial aggregate
    ↓
single-thread merge
    ↓
finalize
```

等这版 benchmark 完成后，如果：

```text
GLOBAL aggregation scaling 好
GROUP BY high-cardinality scaling 差
```

再加入：

```text
radix partition
+
parallel merge
```

这样设计边界是干净的。

---

# 十三、`ParallelScanSession` 和 `BatchStream` 的处理

完成重构以后：

```text
src/storage/scan/parallel_scan_session.h
src/storage/scan/parallel_scan_session.cpp
```

应该删除。

如果确认没有其他使用者：

```text
src/storage/scan/batch_stream.h
```

也删除。

同时：

```cpp
TableStorage::CreateParallelScan()
```

删除。

这不是“保留以后可能有用”。

这套模型和新的模型是互斥的：

```text
旧模型：
storage producer
    ↓
queue
    ↓
execution consumer

新模型：
execution worker
    ↓
storage scan
    ↓
execution pipeline
```

保留两个只会把代码重新搞乱。

---

# 十四、ParallelConfig 也应该重构

现在：

```cpp
struct ParallelConfig {
    size_t scan_threads = 2;
    size_t compute_threads = 4;
    size_t batch_queue_capacity = 64;
};
```

这是旧两阶段架构留下来的配置。

改成：

```cpp
struct ParallelConfig {
    size_t worker_threads = 4;

    // 仅普通 SELECT 并行结果回传需要；
    // aggregate pipeline 不使用。
    size_t result_queue_capacity = 64;
};
```

甚至如果普通 SELECT 暂时维持串行，可以进一步只有：

```cpp
struct ParallelConfig {
    size_t worker_threads = 4;
};
```

因为已经不存在：

```text
scan threads
compute threads
```

之分。

只有：

> pipeline worker。

---

# 十五、Connection 也要同步修改

你现在：

```cpp
const size_t worker_count =
    std::max<size_t>(
        1,
        database_.GetConfig()
            .parallel_config.compute_threads);
```

改成：

```cpp
const size_t worker_count =
    std::max<size_t>(
        1,
        std::min(
            config.worker_threads,
            database_.GetThreadPool()
                .thread_count()));
```

然后：

```cpp
QueryMemoryContext
```

仍然：

```text
CoordinatorArena
WorkerArena(0)
WorkerArena(1)
...
```

这部分设计可以完整保留。

---

# 十六、CLI 配置同步清理

现在：

```text
--threads
--scan-threads
--compute-threads
--queue
```

建议改成：

```text
--threads N
```

语义：

```text
Database ThreadPool threads
+
最大 pipeline worker 数
```

如果你希望数据库线程池和单查询并行度独立：

```text
--threads N
--query-workers N
```

例如：

```text
--threads 8
--query-workers 4
```

表示数据库线程池有 8 个线程，一条 query 最多用 4 个。

删掉：

```text
--scan-threads
--compute-threads
```

这两个概念以后不应该再存在。

---

# 十七、非 Aggregate 的并行 SELECT 怎么处理

比如：

```sql
SELECT id, value
FROM t
WHERE value > 100;
```

这类查询没有 blocking aggregate sink。

第一阶段可以继续保留一个：

```text
worker pipeline
      ↓
result queue
      ↓
coordinator consumer
```

但是注意，这时只能有**一个队列**：

```text
Scan -> Filter -> Projection
全部已经在 worker 内完成

然后：
final batch -> result queue
```

而不是现在：

```text
Scan
 ↓
queue 1
 ↓
Filter/Project
 ↓
queue 2
 ↓
result
```

所以即使普通 SELECT 暂时保留 `BoundedBlockingQueue<VectorBatch> results`：

> 它也已经从“执行 pipeline 中间边界”变成了“最终结果交接边界”。

这两个性质完全不同。

而你的 aggregate 查询甚至这一个 queue 都不需要。

---

# 十八、最终类关系

完成后应该收敛成：

```text
ExecutionEngine
      │
      ↓
ParallelExecutor
      │
      ├── ParallelScanGlobalState
      │       ├── segment_ids
      │       ├── next_segment
      │       ├── prepared_predicates
      │       └── cancelled
      │
      ├───────────────┐
      ↓               ↓
 worker 0           worker N
      │               │
ExecutorBuilder   ExecutorBuilder
      │               │
      ↓               ↓
 SeqScan           SeqScan
      │               │
LocalScanState   LocalScanState
      │               │
      ↓               ↓
TableStorage::ScanParallel()
      │
      ↓
SegmentReader
      │
      ↓
Filter / Projection
      │
      ↓
HashAggregateState(local)
      │
      └─────────┬─────────
                ↓
       HashAggregateState(global)
                │
              Merge
                │
            NextResult
```

Storage 不创建线程。

Storage 不创建 queue。

Storage 不决定线程数量。

Storage 只负责：

```text
给定 query-global scan state
给定 worker-local scan state
读取下一个 batch
```

我认为这是这次重构最重要的边界。

---

# 十九、建议实际修改文件

这轮可以严格限定到下面这些：

| 文件                                                | 修改                                                         |
| ------------------------------------------------- | ---------------------------------------------------------- |
| `src/storage/scan/parallel_scan_state.h`          | **新增** Global/Local Scan State                             |
| `src/storage/scan/parallel_scan_session.h/.cpp`   | **删除**                                                     |
| `src/storage/scan/batch_stream.h`                 | 无其他使用后**删除**                                               |
| `src/storage/table/table_storage.h/.cpp`          | 增加 `CreateParallelScanState/ScanParallel`；增加 reader 复用扫描入口 |
| `src/execution/scan/seq_scan.h/.cpp`              | `BatchStream` → Global/Local Scan State                    |
| `src/execution/executor_builder.h/.cpp`           | `scan_stream` → `parallel_scan`                            |
| `src/execution/parallel/parallel_executor.h/.cpp` | 重写 worker 调度                                               |
| `src/parallel/parallel_config.h`                  | `scan_threads + compute_threads` → `worker_threads`        |
| `src/main/connection.cpp`                         | WorkerArena 数量同步                                           |
| `src/main/execution_context.h`                    | 更新配置说明                                                     |
| `src/execution/execution_context.h`               | 更新配置说明                                                     |
| `src/main.cpp`                                    | 删除旧 CLI 参数                                                 |
| `test/global_aggregate_test.cpp`                  | 扩充并行回归测试                                                   |

---

# 二十、实现顺序

我建议严格按下面顺序改，避免一次性拆爆执行链：

1. **加入 `ParallelScanGlobalState / ParallelScanLocalState`**，先不删除旧实现；给 `TableStorage` 加 `ScanParallel()`，单独写 storage test 验证多个 worker 不重复、不漏 segment。
2. **修改 `SeqScanOperator + ExecutorBuilder`**，增加 direct parallel scan 模式，让 worker pipeline 可以不经过 `BatchStream`。
3. **重写 `ParallelExecutor::ExecuteAggregate()`**，直接 `SeqScan → Filter → Project → local HashAggregateState`；这一步完成后 aggregate 路径已经完全不经过 queue。
4. **加入 SegmentReader local cache**，确保同一 worker 扫一个 segment 时只做一次 `GetSegmentReader()`。
5. **跑现有 `global_aggregate_test`**，重点验证 empty/global/grouped/filter 0%/50%/100% 的 single == multi。
6. **增加并行特有测试**：0/1/2/N segments、threads > segments、多线程同时查询同一 Table、metadata pruning、worker exception/cancel。
7. **删除 `ParallelScanSession / BatchStream / CreateParallelScan`**。
8. **清理 ParallelConfig/CLI**，统一为 pipeline worker。
9. **最后再改普通非 aggregate SELECT**，去掉 scan→compute queue；最终结果 queue 可以暂时保留。
10. **重新测 QPS/perf**，重点观察 `futex`、context-switch、mutex contention、cycles/query，以及 1/2/4/8 worker scaling。

---

## 最终我建议你把这次重构定成一个非常明确的原则

不是：

```text
Storage 多线程
+
Execution 多线程
```

而是：

```text
一个 Query 创建 N 个 pipeline worker

每个 worker：

Claim Segment
    ↓
Scan Segment
    ↓
Filter
    ↓
Projection
    ↓
Partial Aggregate
    ↓
Claim Next Segment
```

最后：

```text
N × Partial Aggregate
        ↓
      Merge
        ↓
     Finalize
```

**Storage 本身不拥有 scan thread；Storage 只是 pipeline worker 调用的数据源。**

你之前 `SegmentSource + local partial aggregate` 的想法，本质方向确实比现在的 `ParallelScanSession + BoundedBlockingQueue` 更接近 DuckDB。现在真正应该做的是把它重新实现成 **query-local Global Scan State + worker-local Local Scan State**，这样既恢复了那个高效路径，又解决了你旧设计中跨 query 共享 segment allocator 的问题。
