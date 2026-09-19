我重新按当前 `master` 的实际代码检查了 `VectorBatch → StorageRowFilter → FilterOperator → ProjectionOperator → HashAggregateState` 这条链。你现在非常适合把这两项一起做，因为第 3 项实际上依赖第 2 项先把“有效行集合”从 `sel_vector` 升级为统一的 bitmap 表达。

我建议把这次改造定义成一个完整阶段：

> **Mask-native execution + Sparse typed projection**

核心原则只有一个：**`SelectionMask` 成为执行期唯一权威的行选择状态，不再同时维护 `SelectionMask` 和 `sel_vector` 两份真源。**

---

# 一、改造后的总体数据流

当前是：

```text
Segment Scan
    ↓ dense batch

Storage Predicate
    ↓
SelectionMask
    ↓ ToSelectionVector()
sel_vector
    ↓

FilterOperator
    ↓ FromSelectionVector()
SelectionMask
    ↓ predicate
SelectionMask
    ↓ ToSelectionVector()
sel_vector
    ↓

Projection
    ├── dense → SIMD
    └── sparse → ExecValue / variant / row-by-row

Aggregate
    ↓
sel_vector[logical]
```

改造后：

```text
Segment Scan
    ↓
VectorBatch
selection = ALL

Storage Predicate
    ↓
SelectionMask
    ↓
VectorBatch.selection

FilterOperator
    ↓
直接读取 batch.selection
    ↓
SelectionMask AND/OR
    ↓
batch.selection

Projection
    ├── Direct Column
    │      → 零拷贝 + 原 mask
    │
    ├── Dense VectorExpression
    │      → 现有 AVX2
    │
    └── Sparse VectorExpression
           → SelectionMask
           → typed gather
           → dense temporary batch
           → 现有 AVX2 arithmetic

Aggregate
    ↓
直接遍历 SelectionMask set bits
```

也就是说正常查询执行过程中不再出现：

```text
mask → vector<uint32_t> → mask
```

---

# 二、`VectorBatch` 改造：SelectionMask 成为一等状态

目前：

```cpp
std::vector<ColumnData> columns;
std::vector<uint32_t> sel_vector;
uint32_t size;
```

建议最终改成：

```cpp
class VectorBatch {
public:
    ...

    uint32_t size = 0; // active row count

    uint32_t PhysicalSize() const noexcept;
    uint32_t ActiveSize() const noexcept;

    bool IsDense() const noexcept;
    bool Empty() const noexcept;

    const simd::SelectionMask& selection() const noexcept;

    void SetIdentitySelection(uint32_t physical_count);
    void SetSelection(const simd::SelectionMask& mask);
    void CopySelectionFrom(const VectorBatch& source);

    void Reset();

    void CompactBySelection();

private:
    simd::SelectionMask selection_;
};
```

我建议**最终直接删除 `sel_vector`，不要长期保留 mask + vector 双状态**。

否则以后非常容易出现：

```text
size = 512
mask.Count() = 512
sel_vector.size() = 493
```

这种执行期状态污染。

新的强不变式定义为：

```text
selection.row_count() == physical row count

size == active row count

dense:
    selection.IsAll() == true
    size == physical_count

sparse:
    selection.IsAll() == false
    size == selection.Count()

所有 ColumnData.count == physical_count
```

经过真正的 materialization/gather 后：

```text
physical_count = active_count
selection = ALL
```

重新成为 dense batch。

---

# 三、扩展 `SelectionMask`：增加高效行遍历能力

这是第 2、3 项共同需要的基础设施。

现在 `SelectionMask` 已经有：

```cpp
Count()
Test()
ToSelectionVector()
```

但以后不能为了遍历行再 `ToSelectionVector()`。

建议增加：

```cpp
class SelectionMask {
public:
    ...

    template <typename Func>
    void ForEachSetBit(Func&& fn) const;

    SelectionCursor MakeCursor() const;
};
```

再增加一个：

```cpp
class SelectionCursor {
public:
    explicit SelectionCursor(const SelectionMask& mask);

    bool Next(uint32_t& row);

    // 一次提取最多 capacity 个 physical row index。
    uint32_t NextBlock(uint32_t* indices,
                       uint32_t capacity);

private:
    const SelectionMask* mask_;
    uint32_t word_index_;
    uint64_t remaining_bits_;
};
```

实现仍然使用你当前已有的技巧：

```cpp
bit = __builtin_ctzll(bits);
bits &= bits - 1;
```

例如：

```text
word 0:
001001010010...

↓ ctz

physical row:
1
4
6
9
...
```

### `SelectionCursor` 的主要消费者

| 模块                    | 用途                |
| --------------------- | ----------------- |
| Aggregate             | 遍历 active rows    |
| ScalarVectorPredicate | 只 Eval 被选中的行      |
| Sparse Projection     | 一批生成 gather index |
| CompactBySelection    | 压缩列               |
| 最终结果打印                | 遍历有效行             |

这样以后任何模块都不需要先产生一个 4KB 的 `vector<uint32_t>`。

---

# 四、Storage 层改造

当前 `StorageRowFilter::Apply()` 最后：

```cpp
if (final_mask.IsAll()) {
    batch.sel_vector.clear();
    batch.size = physical_count;
} else {
    final_mask.ToSelectionVector(batch.sel_vector);
    batch.size = batch.sel_vector.size();
}
```

直接改成：

```cpp
batch.SetSelection(final_mask);
```

即可。

方法语义修改为：

```cpp
static void Apply(
    const PreparedScanPredicates& prepared,
    const std::vector<uint8_t>& row_filter_mask,
    VectorBatch& batch);
```

输入：

```text
batch.selection = ALL
batch.PhysicalSize() = scan_count
```

输出：

```text
batch.selection = predicate result mask
batch.size       = selected count
```

### `SegmentReader::GetVectorBatch()`

完成列 view 后统一：

```cpp
output.SetIdentitySelection(scan_count);
```

而不是：

```cpp
output.sel_vector.clear();
output.size = scan_count;
```

于是从 Storage 创建出来的任何 batch 都天然拥有合法 selection。

---

# 五、FilterOperator 可以明显简化

当前 `FilterOperator` 有：

```cpp
std::vector<uint32_t> selection_;

SelectionMask input_mask_;
SelectionMask result_mask_;
```

改造后只需要：

```cpp
simd::SelectionMask result_mask_;
```

删除：

```cpp
selection_
input_mask_
```

`Next()` 变成逻辑上的：

```cpp
while (child_->Next(batch)) {

    const uint32_t physical_count =
        batch.PhysicalSize();

    prepared_predicate_->Evaluate(
        batch,
        physical_count,
        batch.selection(),
        result_mask_);

    if (result_mask_.Empty())
        continue;

    batch.SetSelection(result_mask_);

    return true;
}
```

于是原本：

```text
Storage mask
↓
sel_vector
↓
Filter input_mask
```

整个过程完全消失。

---

# 六、顺便修改 `ScalarVectorPredicate`

这里现在还有一个隐藏的低效点：

```cpp
for (uint32_t row = 0;
     row < physical_count;
     ++row) {

    if (!input_mask.Test(row))
        continue;

    Eval(...);
}
```

如果 1024 行只剩 30 行：

```text
1024 次循环
1024 次 Test()
30 次 Eval()
```

改成：

```cpp
input_mask.ForEachSetBit(
    [&](uint32_t row) {
        if (ExecValueAsBool(expr_->Eval(batch, row))) {
            ...
        }
    });
```

变成：

```text
30 次 active-row iteration
30 次 Eval()
```

这属于第 2 项的一部分，应该一起完成。

---

# 七、Aggregate 改为直接消费 mask

当前：

```cpp
const uint32_t active = ActiveRowCount(batch);

for (uint32_t logical = 0;
     logical < active;
     ++logical) {

    uint32_t physical =
        ActiveRowIndex(batch, logical);

    ...
}
```

`ActiveRowIndex()` 本质依赖：

```cpp
sel_vector[logical]
```

改成统一接口：

```cpp
ForEachActiveRow(
    batch,
    [&](uint32_t physical_row) {
        ...
    });
```

`batch_utils.h` 定义：

```cpp
template <typename Func>
inline void ForEachActiveRow(
    const VectorBatch& batch,
    Func&& fn);
```

内部：

```text
if batch.IsDense()
    普通连续 for
else
    selection.ForEachSetBit()
```

dense 查询仍然保持最简单的：

```cpp
for (uint32_t row = 0; row < n; ++row)
```

不能为了统一接口让 dense Aggregate 每行调用：

```cpp
mask.Test(row)
```

这是非常重要的。

因此实现必须显式保留：

```text
dense fast path
sparse mask path
```

---

# 八、Direct Projection：保持零拷贝，不做 gather

当前：

```sql
SELECT id, value
FROM table
WHERE ...
```

Projection 已经可以：

```cpp
CopyFrom(src.buffer, src.count, true);
```

这是非常好的设计，不应为了 sparse execution 强制 materialize。

改造后：

```cpp
output.CopySelectionFrom(input_);
```

即可。

也就是：

```text
input:

physical = 1024
active   = 273
mask     = ...

        ↓ direct projection

output:

仍然 physical = 1024
active         = 273
相同 mask
column         = mmap view
```

保持真正的零拷贝。

因此：

```cpp
ProduceViewProjection()
```

仍然作为最高优先级 fast path。

---

# 九、第 3 项核心：增加 Typed Gather Kernel

现在真正的问题位于：

```cpp
ProjectionOperator::Next()
```

当前：

```cpp
if (input_.sel_vector.empty())
    ProduceVectorizedProjection();

return ProduceMaterializedProjection();
```

等价于：

```text
有 Filter
   ↓
只要 sparse
   ↓
整个 Projection SIMD 失效
```

这个需要彻底改掉。

建议新建：

```text
src/simd/gather_kernel.h

src/simd/scalar/gather_scalar.cpp
src/simd/avx2/gather_avx2.cpp
```

定义：

```cpp
struct GatherKernels {

    using GatherI32Fn = void (*)(
        const int32_t* src,
        const SelectionMask& selection,
        int32_t* dst);

    using GatherI64Fn = ...;
    using GatherF32Fn = ...;
    using GatherF64Fn = ...;

    GatherI32Fn i32 = nullptr;
    GatherI64Fn i64 = nullptr;
    GatherF32Fn f32 = nullptr;
    GatherF64Fn f64 = nullptr;
};
```

并加入现有：

```cpp
KernelRegistry
```

形成：

```text
CompareKernels
ArithmeticKernels
GatherKernels
```

---

# 十、AVX2 Gather 的实现方式

### INT32 / FLOAT

每次从 `SelectionCursor` 取 8 个 physical index：

```cpp
alignas(32) uint32_t indices[8];
```

然后：

```cpp
__m256i index =
    _mm256_load_si256(...);

__m256 result =
    _mm256_i32gather_ps(
        src,
        index,
        sizeof(float));
```

INT32 对应：

```cpp
_mm256_i32gather_epi32
```

一次：

```text
8 个任意 physical row
           ↓
8 个连续 output row
```

### INT64 / DOUBLE

每次 4 行：

```cpp
_mm256_i32gather_epi64
_mm256_i32gather_pd
```

尾部不足一个 SIMD 宽度：

```text
scalar gather
```

即可。

### VARCHAR

你的 VARCHAR 是固定 64-byte slot。

不要使用 AVX2 gather。

直接：

```cpp
memcpy(dst + logical * 64,
       src + physical * 64,
       64);
```

作为 scalar fixed-width gather。

---

# 十一、不要让 Projection 每个表达式重复 gather

例如：

```sql
SELECT
    id,
    value,
    value + 1,
    value - 2
FROM t
WHERE ...
```

错误设计会：

```text
gather id
gather value
gather value
gather value
```

应该只 gather：

```text
id
value
```

一次。

因此给 `ProjectionOperator` 增加：

```cpp
VectorBatch gathered_input_;

std::vector<uint32_t> gather_slots_;
```

构造：

```cpp
ProjectionOperator(..., BufferPool* pool)
    : ...
      input_(pool, true),
      gathered_input_(pool, false) {}
```

---

# 十二、VectorExpression 需要暴露依赖列

当前：

```cpp
class VectorExpression {
public:
    virtual void EvaluateDense(...) const = 0;
};
```

增加：

```cpp
virtual void CollectInputSlots(
    std::vector<uint32_t>& slots) const = 0;
```

例如：

```text
value + id
```

返回：

```text
[value_slot, id_slot]
```

而：

```text
value + 10
```

返回：

```text
[value_slot]
```

`ProjectionOperator::Init()` 在编译表达式时一次计算所有需要 gather 的列。

最终：

```text
gather_slots_

例如：
[0, 2, 5]
```

去重后永久保存。

热路径不再分析表达式。

---

# 十三、Sparse Projection 的具体执行路径

建议增加：

```cpp
bool ProduceSparseVectorizedProjection(
    VectorBatch& output);

void BuildGatheredInput(
    const VectorBatch& input);
```

执行过程：

```text
input
physical = 1024
active   = 137
mask     = sparse
       ↓

SelectionCursor
       ↓

Gather slot 0
Gather slot 2
Gather slot 5
       ↓

gathered_input_
physical = 137
active   = 137
selection = ALL
       ↓

现有 EvaluateDense()
       ↓
AVX2 ArithmeticKernel
       ↓

output
physical = 137
active   = 137
selection = ALL
```

这有一个非常重要的好处：

**你现有的 ArithmeticVectorExpression / ArithmeticKernels 完全不用重新设计。**

只是把：

```text
sparse physical input
```

提前转换成：

```text
dense typed input
```

然后继续复用现在已经验证过的 AVX2 路径。

---

# 十四、Projection 三条路径最终确定

`ProjectionOperator::Next()` 应明确成为：

```text
                     child batch
                         │
            ┌────────────┴────────────┐
            │                         │
      all_direct_columns         contains expression
            │                         │
            ▼                         ▼
     ViewProjection             input.IsDense()?
    zero-copy + mask              │
                            ┌─────┴─────┐
                           yes           no
                            │             │
                            ▼             ▼
                     Dense SIMD      Gather + SIMD
```

对应接口建议：

```cpp
bool ProduceViewProjection(
    VectorBatch& output);

bool ProduceDenseProjection(
    VectorBatch& output);

bool ProduceSparseProjection(
    VectorBatch& output);

void GatherRequiredColumns();
```

原来的：

```cpp
ProduceMaterializedProjection()
```

只保留给真正无法 vectorize 的：

```text
SCALAR_EXPRESSION
```

而不是“只要有 selection 就全部 scalar”。

---

# 十五、混合 Projection 的处理

例如：

```sql
SELECT
    id,
    value + 1,
    some_non_vectorizable_expr(...)
FROM t
WHERE ...
```

执行：

```text
selection mask
      ↓
gather id
gather value
      ↓

id
→ typed gather

value + 1
→ Gathered value
→ AVX2 arithmetic

scalar expression
→ 直接遍历 SelectionMask
→ expr->Eval(input_, physical_row)
```

最终所有输出都是：

```text
active_count 行连续数据
```

所以：

```cpp
output.SetIdentitySelection(active_count);
```

这样即便存在一个 scalar expression，也不会迫使另外两个列退回 `ExecValue`。

这是这次 Projection 改造很重要的一点：

> **优化粒度必须保持在 column/slot，而不是整个 ProjectionOperator。**

---

# 十六、`ProjectionSlotPlan` 修改

建议变成：

```cpp
struct ProjectionSlotPlan {

    enum class Kind : uint8_t {
        DIRECT_COLUMN,
        VECTOR_EXPRESSION,
        SCALAR_EXPRESSION,
    };

    Kind kind;

    uint32_t direct_slot = 0;

    std::unique_ptr<VectorExpression>
        vector_expression;

    std::vector<uint32_t>
        required_input_slots;
};
```

不过为了减少每个 slot 的小 vector 分配，我更推荐：

```cpp
required_input_slots
```

只在 `Init()` 时收集到 `ProjectionOperator::gather_slots_`，之后不必长期保存在每个 Plan 中。

---

# 十七、`batch_utils.h` 重新定义职责

当前：

```cpp
ActiveRowCount()
ActiveRowIndex()
PhysicalRowCount()
IsIdentitySelection()
```

改造后建议：

```cpp
ActiveRowCount()
PhysicalRowCount()
ForEachActiveRow()
```

逐步删除：

```cpp
ActiveRowIndex()
IsIdentitySelection()
```

因为前者意味着：

```text
logical index → selection vector lookup
```

后者完全是旧 `sel_vector` 模型的产物。

新的代码不应该再讨论：

```text
logical row index
```

绝大多数执行算子只需要：

```text
physical row
```

或者已经 materialize 后：

```text
dense row
```

---

# 十八、`QueryResult` 与最终输出边界

你全仓目前 `sel_vector` 使用点并不多，所以我建议这次直接迁干净。

`QueryResult::Append...`：

```cpp
copy.CopySelectionFrom(batch);
```

而不是：

```cpp
copy.sel_vector = batch.sel_vector;
```

`PrintBatch()`：

```cpp
ForEachActiveRow(
    batch,
    [&](uint32_t physical_row) {
        PrintRow(...);
    });
```

这样直到真正最终输出阶段，也不需要生成 selection vector。

---

# 十九、`CompactBySel()` 同时升级

当前：

```cpp
CompactBySel()
```

内部：

```cpp
for selected row
    memcpy(...)
```

改成：

```cpp
CompactBySelection()
```

数值类型直接复用新的：

```text
GatherKernels
```

VARCHAR 使用 fixed-width gather。

这样整个工程只有一套：

```text
sparse → dense
```

实现。

不要让：

```text
Projection
Compact
Index
```

分别实现三个不同的 gather 算法。

其中 `IndexScan::GatherRows()` 因为 row id 是 segment 全局 offset、不是 batch mask，第一阶段可以暂时保留现状。

---

# 二十、明确不要在第一版做的内容

这次优化边界建议严格控制：

| 内容                                 | 本阶段 |
| ---------------------------------- | --- |
| SelectionMask 贯穿 Storage/Execution | 做   |
| 删除热路径 mask↔sel_vector 转换           | 做   |
| Aggregate mask iteration           | 做   |
| ScalarPredicate mask iteration     | 做   |
| Direct Projection mask propagation | 做   |
| Numeric typed gather               | 做   |
| AVX2 gather                        | 做   |
| VARCHAR scalar gather              | 做   |
| Sparse VectorExpression            | 做   |
| Global Aggregate fast path         | 暂不做 |
| AVX-512 compress                   | 不做  |
| adaptive selectivity threshold     | 暂不做 |
| expression fusion                  | 不做  |
| JIT                                | 不做  |
| 改存储格式                              | 不做  |
| 改 planner/optimizer 语义             | 不做  |

尤其是 **adaptive gather policy 第一版不要做**。

以后确实可以根据：

```text
active / physical
```

选择：

```text
高选择率：
    对全部 physical rows 做 SIMD
    继续保留 mask

低选择率：
    gather → dense → SIMD
```

但第一版先固定 sparse → gather，更容易验证收益和正确性。

---

# 二十一、推荐实施顺序

1. **先重构 `SelectionMask` 基础能力。** 加 `ForEachSetBit` 和 `SelectionCursor`，补完整的 mask 单元测试，但暂时不改执行语义。

2. **修改 `VectorBatch`。** 加 `selection_`、`SetIdentitySelection()`、`SetSelection()`、`CopySelectionFrom()`、`IsDense()`、`PhysicalSize()`，建立新的强不变式。

3. **迁移 Storage + Filter。** `SegmentReader` 生成 identity mask；`StorageRowFilter` 直接写 batch mask；`FilterOperator` 直接以 batch mask 为输入，不再 `FromSelectionVector/ToSelectionVector`。

4. **迁移剩余消费者并删除 `sel_vector`。** 修改 Aggregate、ScalarVectorPredicate、Direct Projection、QueryResult、PrintBatch、Compact。全仓搜索 `sel_vector` 必须归零。这一步完成后，第 2 项才算真正结束。

5. **加入 GatherKernel。** 先实现 scalar typed gather 并验证 INT32/INT64/FLOAT/DOUBLE/VARCHAR；然后增加 AVX2 gather，通过现有 `KernelRegistry` 运行时派发。

6. **扩展 VectorExpression 和 Projection。** `VectorExpression` 提供输入 slot；`ProjectionOperator::Init()` 建立 `gather_slots_`；新增 `gathered_input_`；实现 Sparse → Gather → Dense SIMD 路径。

7. **最后做 benchmark，而不是边写边调阈值。** 保持原有 dense SIMD benchmark，同时增加 mask-pipeline benchmark 和 sparse-projection benchmark，确认 dense path 无回退后再考虑进一步优化。

---

# 二十二、必须补的测试

这次我建议把验收分成三个层级。

### Selection 层

测试：

```text
row_count:
0
1
4
7
8
31
63
64
65
127
128
1023
1024
```

mask 分布：

```text
ALL
NONE
单 bit
交替 bit
随机 10%
随机 50%
随机 90%
最后一位
跨 uint64 边界
```

验证：

```text
Count
IsAll
Empty
And
Or
AndNot
ForEachSetBit
SelectionCursor
```

### Pipeline 层

至少覆盖：

```sql
WHERE pushed_predicate

WHERE residual_predicate

WHERE pushed AND residual

WHERE A OR B

Filter → Filter

Filter → Direct Projection

Filter → Arithmetic Projection

Filter → Aggregate

Filter → Projection → Aggregate
```

同时跑：

```text
scalar backend
AVX2 backend
single-thread
multi-thread
```

结果必须逐行完全一致。

### Gather 层

针对：

```text
INT32
INT64
FLOAT
DOUBLE
VARCHAR
```

分别测试：

```text
1%
10%
25%
50%
75%
99%
```

选择率。

并强制验证：

```text
scalar gather == AVX2 gather
```

---

# 二十三、Benchmark 设计

建议增加两个专门 benchmark：

```text
benchmark/bench_selection_pipeline.*
benchmark/bench_sparse_projection.*
```

第一个专测：

```sql
SELECT COUNT(*)
FROM t
WHERE value > 0.3
  AND value <= 0.8;
```

构造：

```text
Storage pushed predicate
        ↓
Execution residual predicate
```

旧版正好产生：

```text
Mask
→ SelectionVector
→ Mask
```

因此很适合衡量第 2 项。

第二个测试：

```sql
SELECT value + 1
FROM t
WHERE value >= threshold;
```

threshold 分别构造大约：

```text
1%
10%
25%
50%
75%
90%
```

有效率。

对比：

```text
old:
sparse → ExecValue row loop

new:
sparse → typed gather → AVX2
```

重点采集：

```text
ns / physical row
ns / active row
rows / s
instructions
branches
branch-misses
L1/L3 misses
```

如果环境允许，直接配：

```bash
perf stat
```

---

# 二十四、我认为这次改造最关键的两个不变式

第一：

```text
SelectionMask 是唯一权威 selection representation。
```

不要设计成：

```text
mask 有时候有效
sel_vector 有时候有效
再用一个 enum 表示当前哪个有效
```

这种结构短期方便，长期会让执行层复杂很多。

第二：

```text
Sparse ≠ Scalar。
```

以后整个执行层应该形成：

```text
dense + vectorizable
    → SIMD

sparse + vectorizable
    → gather + SIMD

dense/sparse + non-vectorizable
    → scalar fallback
```

而不是现在：

```text
dense
    → SIMD

sparse
    → everything scalar
```

---

这样改完之后，你的 simple_olap 执行模型会比现在清晰一个层级：

```text
Column Data
+
Selection Bitmap
        ↓
vectorized operators
        ↓
只有真正需要连续输出时才 materialize
```

而且这会为你之后继续做 **Global Aggregate fast path、SIMD Aggregate、late materialization、甚至压缩列执行** 留下很好的基础。第 2 项建议先独立完成并跑通 benchmark，再开始第 3 项；不要两个模块同时大面积修改，否则一旦 SQL 结果出现错位，很难判断是 selection 语义还是 gather 映射出了问题。
