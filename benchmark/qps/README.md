# QPS 三方对比 harness（Compare.md 协议）

用于在**相同 workload / 相同数据集 / 相同计时协议**下对比：

```
simple_queue  (b096352, scan 池 + compute 池 + BoundedBlockingQueue)
simple_direct (8d0906f, morsel-driven pipeline workers)
duckdb        (固定 tag，官方源码构建)
```

结果与结论见 `docs/DuckDBComparison.md`，原始 CSV 见 `results/`。

## 文件

| 文件 | 说明 |
|---|---|
| `qps_workloads.h` | 共享协议：数据集公式、Q1–Q7 SQL、统计、结果消费、CSV |
| `bench_simple_qps.cpp` | simple_olap runner（`-DSIMPLE_OLAP_QUEUE_ARCH` 切旧架构） |
| `bench_duckdb_qps.cpp` | DuckDB runner（需要 `duckdb.hpp` + `duckdb.cpp` amalgamation） |
| `run_compare.sh` | 引擎顺序轮换 + taskset 绑核的批量 runner |
| `results/` | 原始 CSV 与 DuckDB EXPLAIN ANALYZE 输出 |

## 构建 simple runner

```bash
# Release 核心库（新架构）
cmake -S . -B build_release -DCMAKE_BUILD_TYPE=Release -G Ninja \
      -DSIMPLE_OLAP_BUILD_BENCHMARK=OFF
cmake --build build_release --target simple_olap_core -j

# Direct（当前架构，worker_threads）
g++ -std=c++17 -O3 -DNDEBUG -I src -I benchmark/qps \
    benchmark/qps/bench_simple_qps.cpp \
    build_release/lib/libsimple_olap_core.a -lpthread \
    -o /tmp/qps_compare/bench_simple_direct

# Queue（旧架构，scan/compute threads）
git worktree add /tmp/olap_queue b096352
cmake -S /tmp/olap_queue -B /tmp/olap_queue/build -DCMAKE_BUILD_TYPE=Release -G Ninja \
      -DSIMPLE_OLAP_BUILD_BENCHMARK=OFF
cmake --build /tmp/olap_queue/build --target simple_olap_core -j
g++ -std=c++17 -O3 -DNDEBUG -DSIMPLE_OLAP_QUEUE_ARCH \
    -I /tmp/olap_queue/src -I benchmark/qps \
    benchmark/qps/bench_simple_qps.cpp \
    /tmp/olap_queue/build/lib/libsimple_olap_core.a -lpthread \
    -o /tmp/qps_compare/bench_simple_queue
```

## 构建 DuckDB runner（官方 amalgamation）

```bash
git clone --depth 1 --branch v1.5.5 https://github.com/duckdb/duckdb.git /tmp/duckdb
cd /tmp/duckdb && python3 scripts/amalgamation.py
g++ -std=c++17 -O3 -DNDEBUG -fPIC -I src/amalgamation -c src/amalgamation/duckdb.cpp -o duckdb.o
g++ -std=c++17 -O3 -DNDEBUG -I /tmp/duckdb/src/amalgamation -I benchmark/qps \
    benchmark/qps/bench_duckdb_qps.cpp /tmp/duckdb/duckdb.o -lpthread -ldl \
    -o /tmp/qps_compare/bench_duckdb
```

> DuckDB v1.5 起 `SUM/COUNT` 在 `core_functions` 扩展中；benchmark 在计时前
> 按官方方式 `INSTALL core_functions; LOAD core_functions;`。

## 生成数据（Medium = 4,194,304 行）

```bash
B=/tmp/qps_compare
$B/bench_simple_direct --db $B/simple_direct_medium --generate --rows 4194304
$B/bench_simple_queue  --db $B/simple_queue_medium  --generate --rows 4194304
$B/bench_duckdb        --db $B/duckdb_medium.duckdb --generate --rows 4194304

# 正确性：三个引擎结果必须一致（q5 SUM 类型不同、数值相同）
$B/bench_simple_direct --db $B/simple_direct_medium --verify --query all
$B/bench_simple_queue  --db $B/simple_queue_medium  --verify --query all
$B/bench_duckdb        --db $B/duckdb_medium.duckdb --verify --query all
```

## 运行三方矩阵

```bash
BIN_DIR=/tmp/qps_compare DATA_DIR=/tmp/qps_compare \
THREADS="1 2 4 8" WARMUP=2 MEASURE=5 QUERY=all \
bash benchmark/qps/run_compare.sh
```

计时协议：每进程打开 DB、逐查询 warmup 2 s、逐查询测量 5 s（自动迭代数），
**QPS = 完成查询数 / 总墙钟时间**；每轮结果都喂进 order-independent accumulator，
防止查询被优化消除；CSV 额外记录 `getrusage` context switch。
