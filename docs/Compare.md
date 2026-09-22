你现在最值得做的是三方对照：

> **simple_olap 当前 queue 版本 → simple_olap 新 direct-pipeline 版本 → DuckDB**

这样最终你不仅能回答“我比 DuckDB 慢多少”，还能非常清楚地证明：

> 去掉 `BoundedBlockingQueue`、改成 `segment → pipeline → partial aggregate` 后，QPS、P99、context switch、futex 等指标到底改善了多少。

DuckDB 当前可以显式通过 `SET threads = N` 限制查询使用的总线程数，并提供 `EXPLAIN ANALYZE` / profiling 查看算子运行信息，因此很适合作为基准参照。([DuckDB][1])

---

# 一、先确定这次 benchmark 比什么

不要只输出一个“QPS”。

最终报告固定分成 4 组指标：

| 指标                      | 作用                      |
| ----------------------- | ----------------------- |
| QPS                     | 整体查询吞吐                  |
| P50 / P95 / P99 latency | 查询延迟稳定性                 |
| rows/s                  | 扫描/计算吞吐，避免 QPS 被查询数据量误导 |
| CPU 硬件/OS 指标            | 解释为什么快/慢                |

其中 CPU/OS 指标至少记录：

```text
cycles
instructions
IPC
context-switches
cpu-migrations
page-faults
cache-misses
LLC-load-misses
branch-misses
```

另外针对你这次重构，单独记录：

```text
futex syscall 数
mutex contention
```

因为你的优化目标本身就是消灭：

```text
Batch
 ↓
mutex
 ↓
condition_variable
 ↓
Push / Pop
```

---

# 二、三个被测对象必须固定

建议建三个 Git tag / commit：

```text
SIMPLE_QUEUE
当前修改前版本
例如当前 6569fdf...

SIMPLE_DIRECT
完成这次 DuckDB 风格 pipeline 重构后的 commit

DUCKDB
固定一个 DuckDB commit/tag
```

不要今天拿 DuckDB A 测、明天更新成 DuckDB B 再测。

结果文件第一行必须记录：

```text
simple_queue_commit=
simple_direct_commit=
duckdb_commit=
compiler=
compiler_version=
cpu=
kernel=
```

DuckDB 如果源码编译，官方当前 Linux 构建支持 Release/CMake/Ninja；源码构建时也有 `NATIVE_ARCH` 选项，而且默认关闭。([GitHub][2])

---

# 三、编译条件必须对齐

你目前 `simple_olap` 的 Release 是：

```cmake
-O3
```

并且只对 AVX2 kernel：

```text
-mavx2
```

而没有：

```text
-march=native
-ffast-math
```

所以第一组正式成绩建议：

```text
simple_olap:
Release
-O3
现有 AVX2 设置
不加 -march=native

DuckDB:
Release
NATIVE_ARCH=OFF
```

不要：

```text
simple: -march=native
duckdb: 默认
```

或者反过来。

否则结果不好解释。

可以另外做一组：

```text
Native Optimization
```

两个引擎都针对本机编译。

但它属于 supplementary benchmark，不作为主结果。

---

# 四、不要再用“启动一次进程执行一次 SQL”测 QPS

你当前：

```text
bench_execution_modes.sh
```

本质类似：

```text
启动 simple_olap
    ↓
打开数据库
    ↓
建立 Connection
    ↓
执行一条 SQL
    ↓
退出
```

然后重新来。

这测出来是：

```text
process startup
+
database open
+
catalog load
+
query
+
process exit
```

而不是数据库 QPS。

---

## 正确的 QPS 测法

每个 benchmark 进程：

```text
启动进程
 ↓
Open Database          ← 不计时
 ↓
Create Connection      ← 不计时
 ↓
Warmup                 ← 不计时
 ↓
================================
重复执行 SQL N 次       ← 计时
================================
 ↓
统计
 ↓
退出
```

也就是：

```cpp
Database db(...);
Connection con(db);

Warmup();

start = now();

for (...) {
    con.Query(SQL);
}

end = now();
```

DuckDB 同样：

```cpp
duckdb::DuckDB db(path);
duckdb::Connection con(db);

Warmup();

start = now();

for (...) {
    con.Query(SQL);
}

end = now();
```

这是最接近 apples-to-apples 的方式。

---

# 五、我建议增加两个 benchmark executable

你的工程增加：

```text
benchmark/qps/
├── bench_common.h
├── bench_simple_qps.cpp
├── bench_duckdb_qps.cpp
├── workloads.h
└── run_compare.py
```

不要让 Python 执行 DuckDB 查询。

Python只负责：

```text
启动 benchmark
读取 CSV
汇总
```

真正执行 SQL 必须是 C++。

否则：

```text
simple C++ API

vs

DuckDB Python API
```

本身就已经不公平。

---

# 六、两个 executable 必须共享完全相同的 benchmark protocol

定义：

```cpp
struct BenchmarkConfig {
    size_t threads;

    size_t warmup_iterations;

    size_t measure_iterations;

    std::chrono::seconds minimum_duration;

    std::string query_id;

    std::string sql;

    std::string database_path;
};
```

结果：

```cpp
struct BenchmarkResult {
    std::string engine;

    std::string query_id;

    size_t threads;

    uint64_t iterations;

    double total_seconds;

    double qps;

    double mean_us;

    double p50_us;

    double p95_us;

    double p99_us;

    double min_us;

    double max_us;
};
```

每次 query 单独测：

```cpp
auto start = steady_clock::now();

result = connection.Query(sql);

auto end = steady_clock::now();
```

把每次 latency 放进：

```cpp
std::vector<uint64_t> latency_ns;
```

最终：

```text
QPS =
completed_queries / total_seconds
```

注意：

> **QPS 必须直接按总完成查询数 / 总时间算，不要用 `1 / median latency`。**

---

# 七、测量时间不要固定 iterations

因为：

```text
小查询可能 100 us
大查询可能 100 ms
```

如果全部：

```text
100 次
```

一个测 10ms，一个测 10s。

更合理的办法：

### Warmup

固定：

```text
20 次
```

或者至少：

```text
2 秒
```

以先达到者较晚为准。

然后做一次 calibration：

```text
probe latency = T
```

自动选择：

```text
iterations =
clamp(
    10 seconds / T,
    30,
    10000
)
```

也就是说每一个测试配置大约跑：

```text
10 秒
```

正式建议：

```text
warmup ≥ 2 s
measurement ≥ 10 s
```

然后整个 case 重复：

```text
5 round
```

最终报告：

```text
QPS median of 5 rounds
P99 使用全部 query samples
```

这样结果远比“跑 5 次取 best”可靠。

---

# 八、数据集不要继续只有 id/value 两列

建议专门创建：

```sql
CREATE TABLE bench (
    id BIGINT,
    grp INTEGER,
    grp_hi INTEGER,
    value DOUBLE
);
```

生成：

```text
id
= i

grp
= i % 1024

grp_hi
= i % 65536

value
= deterministic_hash(i)
```

其中 `value` 不要真的依赖 RNG 状态。

例如 benchmark generator：

```text
x = (i * 48271) % 1000003

value =
double(x) / 1000003.0
```

这样：

```text
simple_olap
DuckDB
```

可以独立生成数据库，但是数据逐行完全一致。

---

# 九、建立三个规模

我建议：

### Small

```text
100,000 rows
```

用于：

```text
query fixed overhead
parse/bind/plan overhead
极限 QPS
```

### Medium

```text
4,194,304 rows
```

约 400 万。

你的 Segment：

```text
65536 rows
```

所以正好：

```text
64 segments
```

非常适合：

```text
1 / 2 / 4 / 8 threads
```

的扩展性测试。

### Large

```text
33,554,432 rows
```

约 3300 万。

主要测试：

```text
memory bandwidth
multi-core scaling
cache miss
GROUP BY
```

如果你机器内存不大，可以改为：

```text
16M
```

原则是：

> Medium 应完整位于 RAM/page cache；Large 足以产生明显 memory pressure，但不能触发 swap。

---

# 十、正式 SQL workload

不要直接上 TPC-H。

你目前 simple_olap 的 SQL 功能还没有覆盖完整 TPC-H，所以用**能力交集**最合理。

我建议固定下面 7 条。

---

## Q1：Global Aggregate

```sql
SELECT SUM(value)
FROM bench;
```

主要测：

```text
Scan
+
Global Aggregate Fast Path
```

这也是最干净的：

```text
DuckDB pipeline
vs
simple pipeline
```

比较。

---

## Q2：Filter + Aggregate，50% selection

```sql
SELECT COUNT(*), SUM(value)
FROM bench
WHERE value > 0.5;
```

这是**最重要的一条**。

它会同时测试：

```text
scan
predicate
selection mask
AVX2
partial aggregate
parallel merge
```

而结果只有一行，不受结果 materialization 干扰。

我建议把它作为你的：

> **主 QPS 指标**

---

# 十一、Q3：高选择率过滤

```sql
SELECT COUNT(*), SUM(value)
FROM bench
WHERE value > 0.1;
```

约：

```text
90%
```

用来观察 dense path。

---

# 十二、Q4：低选择率过滤

```sql
SELECT COUNT(*), SUM(value)
FROM bench
WHERE value > 0.99;
```

约：

```text
1%
```

用来观察：

```text
SelectionMask
Sparse path
```

---

# 十三、Q5：Expression Aggregate

你现在已经支持：

```sql
SELECT SUM(id + 1)
FROM bench;
```

主要测试：

```text
Scan
 ↓
Expression
 ↓
Aggregate
```

这条能体现你 AVX2 projection/arithmetic 的价值。

---

# 十四、Q6：Low-cardinality GROUP BY

```sql
SELECT grp, COUNT(*), SUM(value)
FROM bench
GROUP BY grp;
```

固定：

```text
1024 groups
```

主要测试：

```text
thread-local hash aggregation
+
partial merge
```

这条对你新的 DuckDB 风格架构非常重要。

---

# 十五、Q7：High-cardinality GROUP BY

```sql
SELECT grp_hi, COUNT(*), SUM(value)
FROM bench
GROUP BY grp_hi;
```

固定：

```text
65536 groups
```

它可以直接暴露你当前：

```text
worker partial hash table
        ↓
single coordinator Merge
```

是不是已经变成瓶颈。

我预期这一项和 DuckDB 的差距会明显变大。

这不是坏事，反而很适合解释：

```text
当前：
single-thread final merge

未来：
radix partition
+
parallel combine
```

为什么有价值。

---

# 十六、结果集查询单独测试

另外增加：

```sql
SELECT id, value
FROM bench
WHERE value > 0.99;
```

但是不要把它混进主 QPS。

因为这一条开始严重受：

```text
QueryResult
VectorBatch 深拷贝
结果 materialization
allocator
```

影响。

标记成：

```text
Materialization Benchmark
```

单独报告：

```text
rows/s
MB/s
latency
```

---

# 十七、不要用 `COUNT(*)` 单独作为核心 benchmark

比如：

```sql
SELECT COUNT(*) FROM bench;
```

容易出现不同引擎的特殊优化。

DuckDB可能通过内部统计/优化得到和你的扫描路径不同的执行方式。

所以：

```text
COUNT(*)
```

可以测试，但不要作为核心排名指标。

更安全的是：

```sql
SELECT SUM(value) FROM bench;
```

或者：

```sql
SELECT COUNT(*), SUM(value)
FROM bench
WHERE value > 0.5;
```

强制真实数据处理。

---

# 十八、线程矩阵

固定测试：

```text
1
2
4
8
```

如果机器物理核 < 8：

```text
1
2
4
physical_core_count
```

不要根据：

```cpp
std::thread::hardware_concurrency()
```

盲目把 SMT 线程也当核心。

先：

```bash
lscpu -e
```

确认：

```text
CPU
CORE
SOCKET
NODE
```

主测试优先只使用**物理核心**。

例如 8C16T：

```text
threads:
1
2
4
8
```

而不是：

```text
16
```

16 可以另做 SMT benchmark。

---

# 十九、DuckDB 每轮必须设置完全相同的线程数

每个 DuckDB benchmark connection 开始：

```sql
SET threads = 1;
```

或者：

```sql
SET threads = 2;
SET threads = 4;
SET threads = 8;
```

DuckDB 官方将 `threads` 定义为系统可以使用的总线程数。([DuckDB][1])

另外建议：

```sql
SET preserve_insertion_order = false;
```

原因是：

你的：

```text
multi-thread simple_olap
```

本来就不承诺没有 `ORDER BY` 时的结果顺序。

DuckDB 默认 `preserve_insertion_order=true`，关闭后允许它对无 `ORDER BY` 的结果重排。([DuckDB][3])

这样两边的语义约束一致。

---

# 二十、DuckDB 内存配置

数据集必须保证 DuckDB 不 spill。

例如机器 32 GB：

```sql
SET memory_limit = '16GB';
```

或者干脆保持一个足够大的上限。

DuckDB 默认 memory limit 与物理 RAM 有关，也支持 `temp_directory` 等 spilling 设置。([DuckDB][4])

正式 benchmark 要记录：

```text
memory_limit
```

但不要让：

```text
DuckDB spill to disk

vs

simple 全内存/page cache
```

发生。

---

# 二十一、主测试必须是 Hot Cache

QPS 的主结果定义：

> **Hot Database / Hot Page Cache QPS**

流程：

```text
打开 DB

执行 Q1 20 次
执行 Q2 20 次
...
```

然后才正式测。

这样主要比较：

```text
execution engine
vectorized execution
parallel scheduling
aggregation
memory access
```

而不是磁盘速度。

这也是你的优化真正针对的东西。

---

# 二十二、Cold Cache 单独测试

可以增加：

```text
Cold First Query Latency
```

但是不要叫 QPS。

流程：

```bash
sync
echo 3 | sudo tee /proc/sys/vm/drop_caches
```

然后：

```text
打开 database
执行一次 query
```

输出：

```text
cold latency
```

这项测试：

```text
mmap
filesystem
compression/decompression
storage format
```

影响很大。

应该和 Hot QPS 分开。

---

# 二十三、特别重要：结果必须被真正消费

不能：

```cpp
auto result = con.Query(sql);
```

然后编译器/接口实际上没有完全消费结果。

simple：

```cpp
QueryResult result =
    connection.Query(sql);

Consume(result);
```

DuckDB：

```cpp
auto result =
    connection.Query(sql);

Consume(result);
```

`Consume()` 至少计算：

```text
row_count
+
checksum
```

例如：

```cpp
uint64_t checksum = 0;

for every result row:
    checksum ^= ...
```

并把最终 checksum 写入一个：

```cpp
volatile / DoNotOptimize
```

目标。

同时两边启动时先运行一次结果验证：

```text
simple result == DuckDB result
```

**结果不一致的 query 不允许进入 benchmark。**

---

# 二十四、区分两种 QPS

这一点非常重要。

## QPS-A：End-to-End SQL QPS

计时包含：

```text
SQL string
 ↓
Lexer/Parser
 ↓
Binder
 ↓
Planner
 ↓
Optimizer
 ↓
Physical Plan
 ↓
Execution
 ↓
QueryResult materialization
```

你的调用：

```cpp
connection.Query(sql);
```

DuckDB：

```cpp
connection.Query(sql);
```

这是最终对外最有意义的 QPS。

---

## QPS-B：Execution-only

你还应该增加一个：

```text
Execution-only benchmark
```

simple_olap：

```text
提前：
parse
bind
logical plan
optimizer
physical plan

计时：
ExecutionEngine::Execute()
```

DuckDB不能简单拿普通 `Query()` 来和这个数字比较。

因此这项主要用于：

```text
simple_queue
vs
simple_direct
```

而不是拿来宣称：

```text
simple 比 DuckDB 快
```

所以报告明确写：

```text
End-to-End:
Simple vs DuckDB

Execution-only:
Simple Queue vs Simple Direct
```

这样非常严谨。

---

# 二十五、QPS测试矩阵

最终主矩阵：

| Dataset | SQL            | Threads |
| ------- | -------------- | ------- |
| Small   | Q1–Q7          | 1       |
| Small   | Q1–Q7          | 4       |
| Medium  | Q1–Q7          | 1       |
| Medium  | Q1–Q7          | 2       |
| Medium  | Q1–Q7          | 4       |
| Medium  | Q1–Q7          | 8       |
| Large   | Q1/Q2/Q5/Q6/Q7 | 1       |
| Large   | Q1/Q2/Q5/Q6/Q7 | 4       |
| Large   | Q1/Q2/Q5/Q6/Q7 | 8       |

每一格：

```text
Simple Queue
Simple Direct
DuckDB
```

三个结果。

---

# 二十六、核心结果表

最终应该长这样：

### Medium / Q2 / 4 Threads

```text
SELECT COUNT(*), SUM(value)
FROM bench
WHERE value > 0.5;
```

| Engine        | QPS | P50 | P95 | P99 | rows/s |
| ------------- | --: | --: | --: | --: | -----: |
| Simple Queue  |     |     |     |     |        |
| Simple Direct |     |     |     |     |        |
| DuckDB        |     |     |     |     |        |

然后：

```text
Direct vs Queue QPS:
+xx%

Direct vs Queue P99:
-xx%

Simple Direct / DuckDB:
0.xx
```

注意：

不要写：

```text
DuckDB 比 simple 快 xx%
```

只写原始比值：

```text
simple_direct_qps / duckdb_qps
```

更清楚。

---

# 二十七、第二张表专门看线程扩展

例如 Q2：

| Threads | Simple QPS | Simple Speedup | DuckDB QPS | DuckDB Speedup |
| ------: | ---------: | -------------: | ---------: | -------------: |
|       1 |            |          1.00x |            |          1.00x |
|       2 |            |                |            |                |
|       4 |            |                |            |                |
|       8 |            |                |            |                |

这个指标甚至比绝对 QPS 更重要。

因为你的新架构目标就是：

```text
1 → 2 → 4 → 8 core
```

尽量扩展。

---

# 二十八、专门验证 Queue 被干掉的 perf 测试

选：

```text
Medium
Q2
4 threads
```

执行：

```bash
perf stat \
  -e cycles,instructions \
  -e context-switches,cpu-migrations,page-faults \
  -e branches,branch-misses \
  -e cache-references,cache-misses \
  ./bench_simple_qps ...
```

分别跑：

```text
Simple Queue
Simple Direct
DuckDB
```

重点看：

```text
context-switches
cycles/query
instructions/query
IPC
cache-misses
```

然后额外诊断 futex。

如果系统提供相应 tracepoint：

```bash
perf stat \
  -e syscalls:sys_enter_futex \
  ...
```

或者只做诊断：

```bash
strace -c -e futex ...
```

注意：

> `strace` 结果不能用于 QPS 成绩。

因为它自己会严重扰动程序。

它只负责证明：

```text
Queue version:
大量 futex

Direct version:
接近消失
```

---

# 二十九、建议你做一个非常漂亮的 before/after 验证

选固定：

```text
4M rows
4 threads
Q2
```

然后记录：

```text
                Queue      Direct     DuckDB

QPS
P50
P99

cycles/query
instructions/query

context switches/query
futex/query

LLC miss/query
```

如果我们的判断正确，你应该看到：

```text
Queue
   ↓
Direct

context switches     显著下降
futex                显著下降
P99                  显著下降
QPS                  上升
```

而：

```text
instructions
```

也应该有所下降，因为大量：

```text
queue push/pop
move
condition variable
synchronization
```

不再存在。

---

# 三十、CPU运行环境固定

每轮 benchmark 前记录：

```bash
uname -a
lscpu
free -h
gcc --version
```

Linux建议：

```text
关闭大型后台程序
接电源
避免 thermal throttling
```

如果允许：

```bash
sudo cpupower frequency-set -g performance
```

整个测试期间不要改 governor。

---

# 三十一、CPU affinity

如果机器例如：

```text
8 physical cores
16 logical CPUs
```

固定：

```bash
taskset -c 0-3
```

来做 4-thread benchmark。

DuckDB：

```text
taskset -c 0-3
SET threads=4
```

simple：

```text
taskset -c 0-3
--threads 4
```

否则：

```text
scheduler
CPU migration
其他 core
```

都会增加噪声。

如果机器是 NUMA，多 socket，则再固定：

```bash
numactl --cpunodebind=0 --membind=0
```

---

# 三十二、不要按“Simple全部测完→DuckDB全部测完”

这样会受：

```text
CPU 温度
后台任务
频率
page cache
```

影响。

应该交错：

```text
Round 1:
Simple Queue
DuckDB
Simple Direct

Round 2:
DuckDB
Simple Direct
Simple Queue

Round 3:
Simple Direct
Simple Queue
DuckDB
```

至少采用轮换顺序。

最好由：

```text
run_compare.py
```

自动完成。

---

# 三十三、DuckDB profiling 单独进行，不放进正式计时

正式测试：

```text
profiling OFF
```

跑完以后针对 Q2/Q6/Q7：

```sql
EXPLAIN ANALYZE
SELECT ...
```

DuckDB会给出 operator 的实际 cardinality 和运行时间；多线程情况下各 operator 的累计时间可能大于查询 wall-clock，因为多个线程同时工作。([DuckDB][5])

你可以拿它来分析：

```text
TABLE_SCAN
FILTER
HASH_GROUP_BY
UNGROUPED_AGGREGATE
PROJECTION
```

然后与你自己的：

```text
Scan
Filter
Projection
PartialAggregate
Merge
Finalize
```

打点对比。

---

# 三十四、simple_olap 同样增加内部阶段计时，但默认关闭

建议：

```cpp
struct QueryProfile {
    uint64_t scan_ns;
    uint64_t filter_ns;
    uint64_t projection_ns;
    uint64_t partial_aggregate_ns;
    uint64_t merge_ns;
    uint64_t finalize_ns;
};
```

但：

```text
正式 QPS：
profiling disabled

分析 benchmark：
profiling enabled
```

不要在正式 QPS 热路径一直：

```cpp
steady_clock::now()
```

否则本身有开销。

---

# 三十五、最终 benchmark 分为 5 个 suite

我建议以后你的 `benchmark/` 最终形成：

```text
Suite A
Correctness
Simple == DuckDB result

Suite B
End-to-End QPS
Simple Direct vs DuckDB

Suite C
Architecture A/B
Simple Queue vs Simple Direct

Suite D
Parallel Scaling
1 / 2 / 4 / 8 threads

Suite E
Perf Analysis
cycles / IPC / cache / context-switch / futex
```

这样你的性能测试体系就完整了。

---

# 三十六、最值得关注的三个 Query

如果第一阶段不想一次搞太大，我建议只做：

### Benchmark 1

```sql
SELECT SUM(value)
FROM bench;
```

测试：

```text
scan + global agg
```

### Benchmark 2 —— 主指标

```sql
SELECT COUNT(*), SUM(value)
FROM bench
WHERE value > 0.5;
```

测试：

```text
scan
+
AVX2 filter
+
selection mask
+
partial aggregate
+
merge
```

### Benchmark 3

```sql
SELECT grp, COUNT(*), SUM(value)
FROM bench
GROUP BY grp;
```

测试：

```text
scan
+
thread-local hash aggregation
+
merge
```

数据：

```text
4,194,304 rows
```

线程：

```text
1 / 2 / 4 / 8
```

引擎：

```text
Simple Queue
Simple Direct
DuckDB
```

光这套 **3 queries × 4 thread-counts × 3 engines**，已经足够准确判断你这次多线程架构重构是否成功。

---

## 我建议的最终目录结构

```text
benchmark/
├── qps/
│   ├── bench_common.h
│   ├── workload.h
│   ├── dataset_generator.h
│   ├── bench_simple_qps.cpp
│   ├── bench_duckdb_qps.cpp
│   └── run_compare.py
│
├── results/
│   ├── raw/
│   └── summary/
│
├── perf/
│   └── run_perf.sh
│
└── existing micro benchmarks...
```

输出统一 CSV：

```text
engine,
commit,
dataset,
rows,
query,
threads,
round,
iterations,
seconds,
qps,
mean_us,
p50_us,
p95_us,
p99_us,
min_us,
max_us
```

这样以后你做：

```text
AVX2
Arena
BufferPool
Delta encoding
新的并行 Aggregate
```

都不需要再重新设计性能测试系统，只是在相同 benchmark framework 中加入一个新 commit 做 A/B。

**尤其对你目前的项目，我不建议第一件事就是追求“Simple QPS / DuckDB QPS 到底是多少”。第一阶段最重要的三个数字应该是 `Queue → Direct` 的 QPS 提升、P99 降幅和 context-switch/futex 降幅；然后再看 `Direct / DuckDB` 的绝对差距。** 这样既能验证这次架构判断，也能定位剩余差距到底是在执行引擎、聚合、存储还是计划阶段。
