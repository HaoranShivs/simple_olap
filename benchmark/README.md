# benchmark

性能基准测试。

## 职责

- 生成测试数据集（不同规模、不同数据分布：均匀/偏斜/低基数）
- 运行典型 OLAP 查询负载（扫描、过滤、聚合、GROUP BY）
- 度量指标：
  - 查询吞吐（rows/s、queries/s）
  - 端到端延迟（p50/p95/p99）
  - 压缩率（各 encoding 的压缩比）
  - 并行加速比（线程数 vs 吞吐）
- 输出可对比的基准报告

## 设计要点

- 基准用例与 test 分离：test 验证正确性，benchmark 验证性能
- 数据集生成可复用（固定随机种子，保证可复现）
- 对比基线：单线程 vs 多线程、不同编码、不同 batch size

## bench_parallel_scan：并行扫描 vs 串行扫描

对比 `TableStorage::Scan`（单线程）与 `ParallelScanSession`（多线程 scan worker
+ 有界队列）的运行时间，输出 best / median / mean、rows/s 与相对串行的加速比。

```bash
./benchmark/run_bench.sh                       # 默认 400 万行，全场景
./benchmark/run_bench.sh --rows 8000000 --threads 1,2,4,8
./benchmark/run_bench.sh --scenario filter --repeats 7
./build/bin/bench_parallel_scan --help         # 全部选项
```

两个场景：

- `scan`：无谓词，串行路径基本是 mmap 零拷贝建视图，单线程极快。
  并行路径要付出线程调度 + 有界队列 push/pop 的开销，因此更容易变慢。
- `filter`：下推 `value > 0.5`（value 在 [0,1) 均匀分布，metadata 无法剪枝），
  每行都进入逐行过滤，是 CPU 密集场景，最能体现多线程加速。

注意事项：

- 串行 `ScanCursor` 把进度（segment 下标）保存在游标自身，同一个 `TableStorage`
  实例可以被反复、并发地扫描。benchmark 在计时区间外只 `Open` 一次，串行/并行
  复用同一实例，排除重复打开的开销与差异。
- 每轮计时仍包含首次懒加载 mmap，用预热轮 + 多轮取 best/median 削弱冷启动与
  系统噪声影响；共享机器上并行数据抖动较大属正常现象。
- 结论方向：只有当每行 CPU 工作（过滤/聚合）占主导时并行才有正收益；纯零拷贝
  扫描会被队列与线程开销吃掉。

## bench_execution_modes：单线程 vs 多线程执行整条 SQL

对「整条 SQL 语句」在 `--mode single` 与 `--mode multi` 下分别重复计时，
输出 best / median / mean 与加速比。执行模式由 `ExecutionEngine` 的
`ExecutionMode` 控制（见 `src/execution/execution_engine.h`）。

脚本会在独立数据库目录中生成一份 `(id INT64, value DOUBLE)` 数据集
（`value ~ U(0,1)`，与 `bench_parallel_scan` 同一种子），默认只做只读查询，
不污染项目主库 `database/`。

```bash
./benchmark/bench_execution_modes.sh                          # 默认 400 万行，5 轮
./benchmark/bench_execution_modes.sh --rows 1000000 --batch 4096 --queue 32
./benchmark/bench_execution_modes.sh --threads 8 --repeats 7 --warmup 3
./benchmark/bench_execution_modes.sh \
    --sql "SELECT COUNT(*) FROM bench_data WHERE value > 0.5;"
./benchmark/bench_execution_modes.sh --no-gen --db "$(pwd)/database" \
    --sql "SELECT id, price FROM goods WHERE price > 500;"
./benchmark/bench_execution_modes.sh --no-build
```

可调参数（默认值与 `benchmark/bench_parallel_scan.cpp` 对齐）：

| 参数 | 默认 | 含义 |
| --- | --- | --- |
| `--rows N` | 4000000 | 数据集行数 |
| `--batch B` | 8192 | 写入侧 DataChunk 行数 |
| `--queue Q` | 16 | 并行批队列容量（背压） |
| `--threads N` | nproc | 多线程模式 compute/scan worker 数 |
| `--repeats R` | 5 | 每个模式计时轮数 |
| `--warmup W` | 2 | 每轮计时前预热次数 |
| `--table NAME` | bench_data | 生成的数据集表名 |
| `--db DIR` | `benchmark/bench_data/modes_db` | 数据库根目录 |
| `--sql "..."` | 内置 3 条 | 追加待测 SQL（可多次） |
| `--no-gen` | - | 跳过数据集生成，复用 `--db` 中已有表 |

脚本依赖的 `simple_olap` CLI 开关：`--db / --gen-table / --rows / --batch /
--drop-table / --queue / --scan-threads / --compute-threads`。
其中 `--threads N` 会同时设置 compute 与 scan 的 worker 数。

要点：

- 借助 `--silent` 跳过逐行结果格式化，避免 I/O 主导计时；同时仍会校验两种模式的
  行数是否一致（`[OK]` / `[MISMATCH]`）。
- 计时包含进程启动与打开 database 的固定开销，脚本会单独测量「仅 exit」基线供扣除。
- 数据集在计时之外生成并 `Flush` 落盘，扫描侧看到的是已封口的 segment。
- 默认数据库目录位于 `benchmark/bench_data/`（已在 `.gitignore` 中忽略）。

参考结果（4 线程，20 万行合成数据集，本机实测）：

| SQL | single | multi | speedup |
| --- | --- | --- | --- |
| `SELECT * FROM bench_data;` | ~3.9 ms | ~8.1 ms | 0.48x |
| `SELECT id, value FROM bench_data WHERE value > 0.5;` | ~7.8 ms | ~8.6 ms | 0.90x |
| `SELECT COUNT(*) FROM bench_data WHERE value > 0.5;` | ~9.9 ms | ~4.5 ms | 2.2x |

结论方向：与 `bench_parallel_scan` 一致 —— 只有每行 CPU 成本占主导的
场景（过滤 + 聚合）多线程才有正收益；纯扫描/投影由于是零拷贝建视图、且
并行路径的消费端（结果收集/深拷贝）仍是单线程，多线程会被调度与队列开销拖慢。
