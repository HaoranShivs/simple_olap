建议把这两项优化做成**两个彼此独立、可以分别 benchmark/回滚的改动**：

* **优化 1：Global Aggregate Fast Path**：无 `GROUP BY` 时彻底绕过 `GroupKey + unordered_map`。
* **优化 4：AVX2 Compare 64-row Bitmap Block**：每 64 行生成一个 `uint64_t` mask，一次写入，配合固定展开。

其中优化 1 不需要改 Planner；优化 4 不需要改 SIMD 对外接口和 `KernelRegistry`。

---

# 一、Global Aggregate Fast Path

当前核心问题位于：

```text
src/execution/aggregate/hash_aggregate_state.h
src/execution/aggregate/hash_aggregate_state.cpp
```

现在即使执行：

```sql
SELECT COUNT(*) FROM t;
SELECT SUM(value) FROM t;
```

`group_exprs_` 为空，`Consume()` 仍然对每一行执行：

```cpp
GroupKey key = EvalGroupKey(...);        // 构造空 key
groups_.try_emplace(...);                // unordered_map lookup
for (...) {
    UpdateAggregate(...);
}
```

而且还有一个顺便应该修掉的语义问题：当前串行 `HashAggregateOperator` 没有调用 `EnsureGlobalGroup()`，因此**空表上的 global aggregate 串行路径和并行路径行为并不完全统一**。

## 1.1 不新增 Operator，不修改 Planner

继续保留：

```cpp
HashAggregateOperator
HashAggregateState
PhysicalHashAggregate
```

不新增 `PhysicalGlobalAggregate`。

只让 `HashAggregateState` 内部拥有两种模式：

```cpp
enum class AggregateMode : uint8_t {
    GLOBAL,
    GROUPED_HASH
};
```

构造时一次确定：

```cpp
mode_ = group_exprs_->empty()
          ? AggregateMode::GLOBAL
          : AggregateMode::GROUPED_HASH;
```

之后热路径中只在 **batch 粒度** 分流一次。

---

## 1.2 `HashAggregateState` 数据成员修改

在 `hash_aggregate_state.h` 中增加：

```cpp
AggregateMode mode_;

StateVector global_states_;

std::pmr::vector<uint32_t> global_count_star_calls_;
std::pmr::vector<uint32_t> global_row_calls_;

bool global_emitted_ = false;
```

原有：

```cpp
GroupTable groups_;
GroupTable::iterator emit_it_;
bool emit_started_;
```

继续保留，只服务于 `GROUPED_HASH`。

最终内部结构变成：

```cpp
HashAggregateState
├── common
│   ├── group_exprs_
│   ├── agg_calls_
│   ├── outputs_
│   └── memory_
│
├── GLOBAL
│   ├── global_states_
│   ├── global_count_star_calls_
│   ├── global_row_calls_
│   └── global_emitted_
│
└── GROUPED_HASH
    ├── groups_
    ├── emit_it_
    └── emit_started_
```

---

## 1.3 构造阶段预编译 global aggregate 调用

构造函数中：

```cpp
HashAggregateState(...)
    : ...
      mode_(group_exprs_->empty()
                ? AggregateMode::GLOBAL
                : AggregateMode::GROUPED_HASH),
      groups_(memory),
      global_states_(memory),
      global_count_star_calls_(memory),
      global_row_calls_(memory) {
```

如果是 `GLOBAL`：

```cpp
global_states_.resize(agg_calls_->size());

for (uint32_t i = 0; i < agg_calls_->size(); ++i) {
    const AggCallSpec& call = (*agg_calls_)[i];

    if (call.type == AggType::COUNT && call.arg == nullptr) {
        global_count_star_calls_.push_back(i);
    } else {
        global_row_calls_.push_back(i);
    }
}
```

这样把：

```sql
COUNT(*)
```

和：

```sql
COUNT(col)
SUM(col)
AVG(col)
MIN(col)
MAX(col)
```

提前分开。

这一分类**只做一次**。

---

# 二、重构 `Consume()`

入口保持：

```cpp
void HashAggregateState::Consume(VectorBatch& batch);
```

修改为：

```cpp
void HashAggregateState::Consume(VectorBatch& batch) {
    if (mode_ == AggregateMode::GLOBAL) {
        ConsumeGlobal(batch);
        return;
    }

    ConsumeGrouped(batch);
}
```

新增：

```cpp
void ConsumeGlobal(VectorBatch& batch);
void ConsumeGrouped(VectorBatch& batch);
```

`ConsumeGrouped()` 基本直接搬当前 `Consume()` 内容，不做额外优化。

因此 GROUP BY 路径行为保持不变。

---

# 三、Global Consume 的具体实现

## 3.1 COUNT(*) 做 batch-level 聚合

这是这次优化里非常重要的一步。

现在：

```sql
SELECT COUNT(*) FROM t;
```

实际上执行的是：

```cpp
for each row:
    ++state.count;
```

修改为：

```cpp
const uint32_t active = ActiveRowCount(batch);

for (uint32_t idx : global_count_star_calls_) {
    global_states_[idx].count += active;
}
```

也就是：

```text
原来：
1024 rows
→ 1024 次 COUNT update

修改：
1024 rows
→ 1 次 count += 1024
```

所以纯 `COUNT(*)` 的聚合状态更新复杂度由：

```text
O(rows)
```

变成：

```text
O(batches)
```

当然 storage 仍然需要扫描数据；你现在的 Storage API 对 `COUNT(*)` 还会读取 hidden driver column，这次暂时不改。

---

# 四、Global Aggregate 的 dense / selected 分流

不要继续使用：

```cpp
for (logical ...) {
    physical = ActiveRowIndex(batch, logical);
}
```

因为这里：

```cpp
sel_vector.empty()
```

本身也是 batch invariant。

直接分成两条循环。

Dense：

```cpp
if (batch.sel_vector.empty()) {
    for (uint32_t row = 0; row < active; ++row) {
        for (uint32_t idx : global_row_calls_) {
            UpdateAggregate(
                (*agg_calls_)[idx],
                global_states_[idx],
                batch,
                row);
        }
    }

    return;
}
```

Filtered：

```cpp
for (uint32_t logical = 0; logical < active; ++logical) {
    const uint32_t physical = batch.sel_vector[logical];

    for (uint32_t idx : global_row_calls_) {
        UpdateAggregate(
            (*agg_calls_)[idx],
            global_states_[idx],
            batch,
            physical);
    }
}
```

因此 global aggregate 热循环中不再有：

```text
GroupKey
PMR GroupKey allocation
HashExecValue
unordered_map::find/try_emplace
ActiveRowIndex() 的 repeated selection branch
```

---

# 五、暂时不要改 `UpdateAggregate()`

本阶段保持：

```cpp
UpdateAggregate(...)
FinalizeAggregate(...)
CombineAggregate(...)
```

语义不变。

尤其不要在这一次顺便把：

```cpp
SUM
AVG
MIN
MAX
```

改成 SIMD 聚合。

这是另一项独立优化。

例如：

```sql
SELECT SUM(id + 1)
```

仍然必须走：

```cpp
call.arg->Eval(...)
```

同样：

```cpp
COUNT(expr)
```

现在仍然执行表达式。

只对：

```cpp
COUNT(*)
```

实施 batch-level count。

这样这一次修改边界非常清楚。

---

# 六、重构 `Merge()`

当前并行模型是：

```text
worker
   ↓
HashAggregateState local
   ↓
future<HashAggregateState>
   ↓
coordinator
   ↓
global.Merge(local)
```

这个架构完全保留。

修改：

```cpp
void HashAggregateState::Merge(HashAggregateState&& other) {
    if (mode_ != other.mode_) {
        throw std::runtime_error(...);
    }

    if (mode_ == AggregateMode::GLOBAL) {
        MergeGlobal(other);
        return;
    }

    MergeGrouped(other);
}
```

Global merge：

```cpp
for (uint32_t i = 0; i < agg_calls_->size(); ++i) {
    CombineAggregate(
        (*agg_calls_)[i],
        global_states_[i],
        other.global_states_[i]);
}
```

这样并行：

```sql
SELECT COUNT(*), SUM(value) FROM t;
```

变成：

```text
worker 0 → [COUNT state, SUM state]
worker 1 → [COUNT state, SUM state]
worker 2 → [COUNT state, SUM state]
worker 3 → [COUNT state, SUM state]

                 ↓

coordinator 顺序 Combine
```

不再：

```text
构造空 GroupKey
复制 GroupKey
unordered_map lookup
try_emplace
```

---

# 七、删除 `EnsureGlobalGroup()`

现在：

```cpp
EnsureGlobalGroup()
```

是用一个：

```cpp
GroupKey{}
```

模拟 global aggregate。

引入 `global_states_` 后，这个概念应该彻底消失。

删除：

```cpp
void EnsureGlobalGroup();
```

并删除 `parallel_executor.cpp` 中两处：

```cpp
local.EnsureGlobalGroup();
global.EnsureGlobalGroup();
```

构造 `HashAggregateState` 时：

```cpp
global_states_.resize(agg_calls_->size());
```

就已经代表一个始终存在的 global group。

这还能顺便统一：

```sql
SELECT COUNT(*) FROM empty_table;
```

在 serial / parallel 两种执行模式下的行为。

---

# 八、Global `NextResult()`

`NextResult()` 同样 batch-level 分流：

```cpp
bool HashAggregateState::NextResult(VectorBatch& output) {
    if (mode_ == AggregateMode::GLOBAL) {
        return NextGlobalResult(output);
    }

    return NextGroupedResult(output);
}
```

Global 永远最多输出 1 行。

核心逻辑：

```cpp
if (global_emitted_) {
    ClearBatch(output);
    return false;
}

ClearBatch(output);

for (const auto& spec : *outputs_) {
    output.AddColumn(spec.type);
    output.columns.back().Resize(1);
}

for (uint32_t col = 0; col < outputs_->size(); ++col) {
    const auto& spec = (*outputs_)[col];

    if (spec.kind != AggregateOutputSpec::Kind::AGGREGATE) {
        throw std::runtime_error(...);
    }

    ExecValue value =
        FinalizeAggregate(
            (*agg_calls_)[spec.index],
            global_states_[spec.index]);

    WriteExecValue(output.columns[col], 0, value);
}

output.size = 1;
output.sel_vector.clear();

global_emitted_ = true;
return true;
```

GROUP BY 的现有 `NextResult()` 搬进：

```cpp
NextGroupedResult()
```

不改变。

---

# 九、Global Aggregate 最终调用链

改完后：

```text
HashAggregateOperator::Next
        │
        ├── child_->Next(batch)
        │
        ▼
HashAggregateState::Consume
        │
        ├── GLOBAL
        │     │
        │     ├── COUNT(*): count += ActiveRowCount(batch)
        │     │
        │     └── SUM/AVG/MIN/MAX:
        │             direct global_states_ update
        │
        └── GROUPED_HASH
              │
              └── 原 Hash GroupTable 路径
```

并行：

```text
worker
  │
  ▼
global_states_[]
  │
  ▼
future<HashAggregateState>
  │
  ▼
CombineAggregate()
  │
  ▼
coordinator global_states_[]
```

---

# 十、优化 4：AVX2 Compare 64-row Bitmap Block

这部分只修改：

```text
src/simd/avx2/compare_avx2.cpp
```

**不修改：**

```text
compare_kernel.h
kernels.h
kernels.cpp
SelectionMask
FilterOperator
StorageRowFilter
```

所以现有调用：

```cpp
kernels.i32_const(...)
kernels.i64_const(...)
kernels.f32_const(...)
kernels.f64_const(...)
```

接口完全保持。

---

# 十一、当前 AVX2 Compare 的问题

INT32/FLOAT 当前每 8 行：

```cpp
movemask
words[base >> 6] |= mask << offset;
```

所以处理 64 行时相当于：

```text
word[0] |= mask0
word[0] |= mask1 << 8
word[0] |= mask2 << 16
...
word[0] |= mask7 << 56
```

即同一个 `uint64_t`：

```text
8 次 read-modify-write
```

INT64/DOUBLE 每次 4 行，因此 64 行是：

```text
16 次 read-modify-write
```

目标改成：

```text
AVX compare
AVX compare
AVX compare
...
     ↓
寄存器中组合 uint64_t
     ↓
words[word] = final_mask
```

即：

```text
64 rows → 1 次 bitmap store
```

---

# 十二、增加小粒度 movemask helper

现有：

```cpp
StoreBitsI32()
StoreBitsI64()
StoreBitsF32()
StoreBitsF64()
```

继续保留给 remainder 使用。

增加只负责返回 mask 的函数，例如：

```cpp
inline uint32_t MaskI32(__m256i cmp) {
    return static_cast<uint32_t>(
        _mm256_movemask_ps(
            _mm256_castsi256_ps(cmp)));
}
```

类似：

```cpp
MaskI64()
MaskF32()
MaskF64()
```

返回：

```text
I32/F32 → 8 bit 有效
I64/F64 → 4 bit 有效
```

---

# 十三、32-bit 类型：8 × AVX2 = 64 rows

对 INT32 / FLOAT：

```text
256 bit
÷ 32 bit
= 8 rows
```

所以一个 bitmap word：

```text
8 AVX operations × 8 rows = 64 rows
```

建议实现：

```cpp
inline uint64_t Compare64I32Const(
    const int32_t* data,
    __m256i rhs,
    CmpOp op);
```

内部逻辑相当于：

```cpp
m0 = compare(data +  0);
m1 = compare(data +  8);
m2 = compare(data + 16);
m3 = compare(data + 24);

m4 = compare(data + 32);
m5 = compare(data + 40);
m6 = compare(data + 48);
m7 = compare(data + 56);
```

然后：

```cpp
uint64_t low =
      uint64_t(m0)
    | uint64_t(m1) << 8
    | uint64_t(m2) << 16
    | uint64_t(m3) << 24;

uint64_t high =
      uint64_t(m4)
    | uint64_t(m5) << 8
    | uint64_t(m6) << 16
    | uint64_t(m7) << 24;

return low | (high << 32);
```

这里故意使用：

```text
low
high
```

两个独立 accumulator。

不要写成：

```cpp
mask |= ...
mask |= ...
mask |= ...
```

否则你又人为建立了一条长 dependency chain。

---

# 十四、64-bit 类型：16 × AVX2 = 64 rows

INT64 / DOUBLE：

```text
256 bit
÷ 64 bit
= 4 rows
```

所以需要：

```text
16 AVX operations
```

建议分成四组：

```cpp
uint64_t q0; // row  0..15
uint64_t q1; // row 16..31
uint64_t q2; // row 32..47
uint64_t q3; // row 48..63
```

每组：

```cpp
q0 =
      uint64_t(m0)
    | uint64_t(m1) << 4
    | uint64_t(m2) << 8
    | uint64_t(m3) << 12;
```

最后：

```cpp
return q0
     | (q1 << 16)
     | (q2 << 32)
     | (q3 << 48);
```

这样避免一个 16 层连续 OR dependency。

---

# 十五、四种类型分别提供 64-row helper

建议明确实现 8 个 helper：

```cpp
Compare64I32Const
Compare64I32Column

Compare64I64Const
Compare64I64Column

Compare64F32Const
Compare64F32Column

Compare64F64Const
Compare64F64Column
```

我不建议在这里过度模板化成一个复杂 generic helper。

这是极热 SIMD kernel，显式代码更容易：

```text
检查生成汇编
控制 register pressure
确认 load / compare / movemask 顺序
定位性能退化
```

---

# 十六、修改主循环

以：

```cpp
CompareI32ConstAvx2()
```

为例。

修改成三级结构：

```cpp
result.SetNone(count);

uint64_t* words = result.data();

const __m256i vrhs = _mm256_set1_epi32(rhs);

uint32_t i = 0;

// 1. 64-row fast path
for (; i + 64 <= count; i += 64) {
    words[i >> 6] =
        Compare64I32Const(data + i, vrhs, op);
}

// 2. 原来的 AVX2 remainder
for (; i + 8 <= count; i += 8) {
    const __m256i v = ...;

    StoreBitsI32(
        words,
        i,
        CmpI32(v, vrhs, op));
}

// 3. scalar tail
for (; i < count; ++i) {
    if (CompareTypedValue(...)) {
        words[i >> 6] |= ...;
    }
}
```

其它 7 个 kernel 使用完全相同结构：

```text
64-row block
    ↓
8/4-row AVX remainder
    ↓
scalar tail
```

---

# 十七、为什么 full block 必须使用 `=`

完整 64-row block：

```cpp
words[i >> 6] = block_mask;
```

不要：

```cpp
words[i >> 6] |= block_mask;
```

因为：

```cpp
result.SetNone(count)
```

已经保证初始为 0。

而且这一段正好覆盖完整的：

```text
word N → row N*64 ... N*64+63
```

因此直接覆盖。

只有 remainder 才继续使用：

```cpp
|=
```

这样 full block 不产生 bitmap read-modify-write。

---

# 十八、不要在这一阶段加入 `__builtin_prefetch`

这两个优化完成时，不要顺手加入：

```cpp
__builtin_prefetch()
```

AVX compare 当前访问模式是：

```text
data[0]
data[1]
data[2]
...
```

完全顺序。

硬件 prefetcher 已经非常容易识别。

如果把：

```text
64-row block optimization
loop unrolling
software prefetch
```

一次全部加入，就很难判断性能变化究竟来自哪里。

这次保持：

```text
Optimization A：Global Aggregate
Optimization B：64-row AVX bitmap
```

两个变量即可。

---

# 十九、`CmpOp` 暂时不模板化

现在：

```cpp
CmpI32(a, b, op)
```

内部是：

```cpp
switch (op)
```

理论上可以进一步做：

```cpp
template<CmpOp OP>
CmpI32(...)
```

然后 kernel 入口只 dispatch 一次：

```cpp
switch (op) {
case EQ:
    CompareI32Impl<CmpOp::EQ>();
...
}
```

但是这应该作为**优化 4 的第二阶段**。

第一阶段先完成：

```text
64-row packing
+
single bitmap store
+
unrolling
```

然后看生成汇编。

如果 GCC/Clang 已经对 invariant `op` 做 loop unswitch，就不需要再增加 48 组模板实例。

如果汇编里仍存在 loop 内 `switch(op)`，再做：

```text
CmpOp dispatch hoisting
```

---

# 二十、测试修改

现有：

```text
test/simd_compare_test.cpp
```

必须扩展。

当前测试已经覆盖：

```text
scalar vs AVX2
INT32
INT64
FLOAT
DOUBLE
6 种 CmpOp
2^53 INT64 精度
SelectionMask
```

但新的 64-row 边界测试要系统化。

测试 `count` 至少包含：

```cpp
0
1
3
4
7
8
15
16
31
32
63
64
65
71
72
127
128
129
1023
1024
```

针对：

```text
INT32
INT64
FLOAT
DOUBLE
```

分别验证：

```text
const compare
column compare
```

每种再跑：

```text
EQ
NE
GT
GE
LT
LE
```

始终要求：

```cpp
AVX2 mask == scalar mask
```

并验证：

```text
[count, 1024) 所有 bit == 0
```

FLOAT / DOUBLE 建议额外加入：

```text
0
-0
NaN
+Inf
-Inf
```

确保重构没有改变当前浮点比较语义。

---

# 二十一、Global Aggregate 正确性测试

新增一组 execution / SQL 测试，重点覆盖：

| SQL 类型               | 必测                              |
| -------------------- | ------------------------------- |
| `COUNT(*)`           | 空表、1 行、大量行                      |
| `COUNT(col)`         | 有数据                             |
| `SUM`                | INT / FLOAT                     |
| `AVG`                | INT / FLOAT                     |
| `MIN/MAX`            | INT / FLOAT                     |
| 多聚合                  | `COUNT(*), SUM(), MIN(), MAX()` |
| WHERE 0%             | 无行命中                            |
| WHERE 100%           | 全命中                             |
| WHERE ~50%           | selection vector 路径             |
| expression aggregate | `SUM(id + 1)`                   |
| GROUP BY             | 确保旧路径无回归                        |
| parallel             | 和 single 输出完全一致                 |

尤其验证：

```sql
SELECT COUNT(*) FROM empty_table;
```

必须：

```text
single == multi
```

并且输出一行：

```text
0
```

---

# 二十二、Benchmark 设计

建议新增：

```text
benchmark/bench_global_aggregate.sh
benchmark/bench_compare_kernel.cpp
```

`bench_global_aggregate.sh` 固定测试：

```sql
SELECT COUNT(*) FROM bench_data;

SELECT COUNT(*) FROM bench_data
WHERE value >= 0.5;

SELECT SUM(id) FROM bench_data;

SELECT SUM(value) FROM bench_data;

SELECT AVG(value) FROM bench_data;

SELECT MIN(id), MAX(id),
       MIN(value), MAX(value)
FROM bench_data;

SELECT COUNT(*), SUM(id), AVG(value),
       MIN(id), MAX(id)
FROM bench_data;
```

并保留一组：

```sql
SELECT id, COUNT(*)
FROM bench_data
GROUP BY id;
```

作为 grouped path regression check。

---

# 二十三、性能验证指标

除了 wall time，建议对这两次优化跑：

```bash
perf stat \
  -e cycles,instructions,branches,branch-misses,\
L1-dcache-load-misses,LLC-load-misses \
  ...
```

Global Aggregate 重点看：

```text
instructions
cycles
IPC
```

因为主要消除：

```text
hash
GroupKey
unordered_map
per-row COUNT update
```

AVX Compare 重点看：

```text
cycles
instructions
cycles / row
```

尤其对：

```text
1024 rows / call
```

做 microbenchmark。

合入条件不建议写死“必须提升 20%”。

更合理的是：

```text
正确性完全一致
+
global aggregate instructions 明显下降
+
64-row AVX kernel cycles/row 下降
+
GROUP BY 不出现统计显著回退
+
小 count compare 不出现明显回退
```

---

# 二十四、最终修改文件范围

建议最终严格控制为：

```text
优化 1：

src/execution/aggregate/hash_aggregate_state.h
src/execution/aggregate/hash_aggregate_state.cpp
src/execution/parallel/parallel_executor.cpp

可选：
test/global_aggregate_test.cpp
benchmark/bench_global_aggregate.sh


优化 4：

src/simd/avx2/compare_avx2.cpp
test/simd_compare_test.cpp

可选：
benchmark/bench_compare_kernel.cpp
CMakeLists.txt
```

**不修改：**

```text
Planner
Optimizer
PhysicalPlan
ExecutorBuilder
FilterOperator
StorageRowFilter
SelectionMask API
CompareKernel API
KernelRegistry API
scalar compare backend
```

这样两项优化都属于**执行层内部性能重构**，不会向上层扩散接口变化。

我建议实际开发顺序就是：

**先完成 Global Aggregate Fast Path，并单独 benchmark；再完成 AVX2 64-row block，并单独 benchmark。不要放在同一个 commit 里。**

其中第 1 项我认为应该优先，因为它不仅消除明显的 hash-table 热路径开销，还顺带把当前 serial/parallel 的空输入 global aggregate 语义统一掉；第 4 项则是一个非常干净的 SIMD micro-kernel 优化。
