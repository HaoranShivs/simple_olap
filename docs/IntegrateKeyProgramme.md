结合现在的 `Catalog -> StorageManager -> TableStorage -> Segment`、`AST -> Binder -> LogicalPlan -> PhysicalPlan -> Execution` 架构，我建议**不要把主键逻辑塞进 Segment**，而是设计成：

> **Catalog 保存“有哪些键”的逻辑定义；TableStorage 维护键索引和唯一性；Segment 仍然只负责列式数据。**

下面我把你说的“键”按普通二级 `KEY` / 索引理解，即**非唯一键**；`PRIMARY KEY` 负责唯一性，同时也可以参与等值查询加速。

---

# 一、第一版功能边界

建议第一版直接支持：

```sql
CREATE TABLE users (
    id INT PRIMARY KEY,
    age INT,
    city VARCHAR,
    KEY idx_age (age)
);
```

同时支持表级主键：

```sql
CREATE TABLE orders (
    user_id INT,
    order_id INT,
    amount DOUBLE,
    PRIMARY KEY (user_id, order_id),
    KEY idx_amount (amount)
);
```

语义定为：

| 类型          | 唯一 | 可多列 | 用于查询加速 | NULL            |
| ----------- | -: | --: | -----: | --------------- |
| PRIMARY KEY |  是 |   是 |      是 | 当前项目无 NULL，暂不考虑 |
| KEY         |  否 |   是 |      是 | 当前项目无 NULL，暂不考虑 |

第一版只让索引优化：

```sql
WHERE key_column = literal
```

或者完整复合键：

```sql
WHERE user_id = 1 AND order_id = 100
```

暂时**不要**实现范围索引、`CREATE INDEX`、`DROP INDEX`、`UNIQUE KEY`、外键、在线重建索引、事务级回滚。

你的 Segment 已经有 min/max pruning，范围查询暂时继续走 SeqScan 即可。

---

# 二、Schema 层：键必须属于 Catalog 元数据

你现在：

```cpp
struct ColumnSchema {
    uint32_t column_id;
    std::string name;
    DataType type;
};

struct TableSchema {
    std::vector<ColumnSchema> columns;
};
```

修改为：

```cpp
enum class KeyType : uint8_t {
    PRIMARY,
    SECONDARY
};

using KeyId = uint32_t;

struct KeySchema {
    KeyId key_id = 0;
    std::string name;
    KeyType type = KeyType::SECONDARY;

    // 支持复合键
    std::vector<ColumnId> columns;
};

struct TableSchema {
    std::vector<ColumnSchema> columns;

    std::optional<KeySchema> primary_key;

    std::vector<KeySchema> secondary_keys;
};
```

这里最重要的是：

**不要在 `ColumnSchema` 里加 `bool primary_key`。**

因为：

```sql
PRIMARY KEY(a, b)
```

是表级约束，而不是单列属性。

`TableCatalogEntry` 不需要增加字段：

```cpp
struct TableCatalogEntry {
    TableId table_id;
    std::string name;
    TableSchema schema;
};
```

键自然跟着 `TableSchema` 持久化。

而：

```cpp
TableStorageMeta
```

仍然只保存：

```cpp
table_id
segment_ids
```

**不要把 KeySchema 放进 `table.meta`。**

这和你现在 Catalog / Storage 的职责划分完全一致。

---

# 三、Catalog 持久化版本升级

你当前 `catalog.meta` 已经是：

```cpp
kCatalogMetaMagic = 0x324C4F43; // COL2
```

加入 KeySchema 后不要直接修改原来的 `TableSchema::Deserialize()` 格式，否则旧数据库会错误解析。

建议升级为：

```cpp
constexpr uint32_t kCatalogMetaMagicV2 = 0x324C4F43; // COL2
constexpr uint32_t kCatalogMetaMagicV3 = 0x334C4F43; // COL3
```

V3：

```text
catalog magic
table count

TableCatalogEntry
    name
    table_id

    column count
        column...

    has_primary_key
        primary key...

    secondary_key_count
        key...
```

读取规则：

```text
COL2
    -> 按旧格式读取 columns
    -> primary_key = nullopt
    -> secondary_keys = {}

COL3
    -> 完整读取 columns + keys
```

这样你原来的数据库仍然可以打开。

---

# 四、Parser / AST

当前 `token.h` 没有 `PRIMARY`、`KEY`。

增加：

```cpp
PRIMARY,
KEY,
```

Lexer 增加：

```cpp
if (text == "PRIMARY")
    return Token{TokenType::PRIMARY, text, start};

if (text == "KEY")
    return Token{TokenType::KEY, text, start};
```

AST 不建议直接存 ColumnId，因为 Parser 阶段还不应该绑定列。

增加：

```cpp
struct KeyConstraint {
    enum class Type {
        PRIMARY,
        SECONDARY
    };

    Type type;
    std::string name;

    std::vector<std::string> columns;
};
```

`CreateTableStatement`：

```cpp
class CreateTableStatement : public Statement {
public:
    std::string table_name;

    std::vector<ColumnSchema> columns;

    std::vector<KeyConstraint> keys;
};
```

这里虽然你当前 Parser 已经给 `ColumnSchema.column_id` 赋值，我建议这一轮顺便把 **column_id 的最终确定移动到 Binder**，Parser 只负责语法。

CREATE TABLE 的解析语法变成：

```text
create_item :=
      column_definition
    | primary_key_definition
    | secondary_key_definition

column_definition :=
    IDENTIFIER DATA_TYPE [PRIMARY KEY]

primary_key_definition :=
    PRIMARY KEY '(' identifier_list ')'

secondary_key_definition :=
    KEY [IDENTIFIER] '(' identifier_list ')'
```

因此：

```sql
id INT PRIMARY KEY
```

最终也统一转换为：

```cpp
KeyConstraint{
    PRIMARY,
    "",
    {"id"}
}
```

后续 Binder 根本不用关心这是 column-level 还是 table-level 写法。

---

# 五、Binder

这是主键语义校验的主要位置。

建议增加：

```cpp
struct BoundKeyDef {
    KeyId key_id;
    std::string name;
    KeyType type;

    std::vector<ColumnId> columns;
};
```

不过结合你现有架构，我更建议进一步简化 `BoundCreateTableStatement`：

```cpp
class BoundCreateTableStatement : public BoundStatement {
public:
    std::string table_name;

    TableSchema schema;
};
```

Binder 在这里一次性把：

```text
列名
    ↓
ColumnId

Key 中的列名
    ↓
ColumnId
```

全部绑定完成。

例如：

```sql
PRIMARY KEY(user_id, order_id)
```

变成：

```cpp
KeySchema {
    .key_id = 0,
    .name = "__primary__",
    .type = KeyType::PRIMARY,
    .columns = {0, 1}
}
```

Binder 必须检查：

```text
只能存在一个 PRIMARY KEY
KEY/PRIMARY KEY 引用的列必须存在
同一个 key 内不能重复列
secondary key 名称不能重复
主键至少包含一个列
普通 key 至少包含一个列
```

---

# 六、CREATE TABLE 链路建议顺便清理

你现在有一个比较明显的层次问题：

```text
PhysicalCreateTable
       ↓
CommandExecutor
       ↓
重新构造 CreateTableStatement
       ↓
Catalog::CreateTable(stmt)
```

也就是：

> PhysicalPlan 又被转换回 AST。

加入 Key 后不建议继续这么做。

改成：

```cpp
bool Catalog::CreateTable(
    std::string table_name,
    TableSchema schema);
```

然后：

```text
AST
 ↓
Binder
 ↓
BoundCreateTableStatement {
    table_name
    TableSchema
}
 ↓
LogicalCreateTable
 ↓
PhysicalCreateTable
 ↓
CommandExecutor
 ↓
Catalog::CreateTable(name, schema)
```

建议：

```cpp
class LogicalCreateTable {
    std::string table_name_;
    TableSchema schema_;
};

class PhysicalCreateTable {
    std::string table_name_;
    TableSchema schema_;
};
```

这样 Key 定义不会在几个层之间重复转换。

---

# 七、真正的键实现：新增 `storage/index`

建议新增：

```text
src/storage/index/
    key_encoder.h
    key_encoder.cpp

    primary_key_index.h
    primary_key_index.cpp

    secondary_key_index.h
    secondary_key_index.cpp

    table_index_manager.h
    table_index_manager.cpp
```

核心结构：

```cpp
struct RowLocation {
    SegmentId segment_id;
    uint32_t row_offset;
};
```

主键：

```cpp
class PrimaryKeyIndex {
public:
    bool Contains(const EncodedKey& key) const;

    void Insert(
        EncodedKey key,
        RowLocation location,
        bool visible);

    std::optional<RowLocation>
    Lookup(const EncodedKey& key) const;

    void MarkSegmentVisible(SegmentId segment_id);

private:
    // 包含 visible + pending，
    // 用于唯一性判断
    std::unordered_map<EncodedKey, IndexEntry> entries_;
};
```

普通 KEY：

```cpp
class SecondaryKeyIndex {
public:
    void Insert(
        EncodedKey key,
        RowLocation location,
        bool visible);

    std::vector<RowLocation>
    LookupEqual(const EncodedKey& key) const;

    void MarkSegmentVisible(SegmentId segment_id);
};
```

再由：

```cpp
class TableIndexManager {
public:
    TableIndexManager(
        const TableSchema& schema);

    void BuildFromPersistedSegments(TableStorage& table);

    void ValidatePrimaryKey(
        const DataChunk& chunk) const;

    void OnAppend(
        const DataChunk& chunk,
        const std::vector<RowLocation>& locations);

    void MarkSegmentsVisible(
        const std::vector<SegmentId>& segments);

    std::vector<RowLocation> Lookup(
        KeyId key_id,
        const EncodedKey& key) const;
};
```

统一管理。

---

# 八、Key 编码不能使用 `double`

这里有一个与你当前代码直接相关的重要问题。

现在 `CommandExecutor::ExecuteInsert()` 中：

```cpp
struct InsertValue {
    bool is_string;
    double number;
    std::string text;
};
```

然后 INT64 也是：

```cpp
static_cast<int64_t>(value.number)
```

主键实现以后，这种方式绝对不能用于 Key。

例如两个不同的 `INT64` 在超过 `2^53` 后转换成 `double` 可能失去区别。

建议建立真正的：

```cpp
using ScalarValue =
    std::variant<
        int32_t,
        int64_t,
        float,
        double,
        std::string>;
```

KeyEncoder 必须按照**实际 DataType**编码：

```cpp
class KeyEncoder {
public:
    static EncodedKey Encode(
        const TableSchema& schema,
        const KeySchema& key,
        const DataChunk& chunk,
        uint32_t row);

    static EncodedKey Encode(
        const TableSchema& schema,
        const KeySchema& key,
        const VectorBatch& batch,
        uint32_t row);
};
```

复合主键：

```text
INT32(10)
VARCHAR("abc")
INT64(100)
```

编码时必须带：

```text
type + length + raw value
```

避免：

```text
("ab", "c")
("a", "bc")
```

发生编码碰撞。

---

# 九、PRIMARY KEY 唯一性应该在 TableStorage 保证

这一点我认为非常重要。

不要写成：

```text
CommandExecutor
    检查主键
    ↓
TableStorage::Append()
```

否则未来 CSV 导入、BulkLoad 等其他写入口可能绕过主键约束。

应该：

```text
CommandExecutor
    ↓
TableStorage::Append(chunk)
        ↓
TableIndexManager::ValidatePrimaryKey()
        ↓
真正 Append
        ↓
TableIndexManager::OnAppend()
```

也就是：

```cpp
void TableStorage::Append(const DataChunk& input) {
    std::lock_guard lock(write_mutex_);

    index_manager_->ValidatePrimaryKey(input);

    auto locations = AppendInternal(input);

    index_manager_->OnAppend(input, locations);
}
```

而：

```cpp
AppendInternal()
```

负责返回：

```cpp
std::vector<RowLocation>
```

因为 TableStorage 自己最清楚：

```text
这一行最终落在哪个 segment
这一行在 segment 中 offset 是多少
```

---

# 十、多行 INSERT 必须先整体检查

例如：

```sql
INSERT INTO t VALUES
(1),
(2),
(1);
```

不能：

```text
插入 1
插入 2
发现第三个重复
报错
```

这样留下半条 SQL 的数据。

正确顺序：

```text
DataChunk
   ↓
生成全部 PrimaryKey
   ↓
检查 batch 内部是否重复
   ↓
检查是否和已有主键重复
   ↓
全部通过
   ↓
Append
```

即：

```cpp
PrimaryKeyIndex::ValidateBatch(...)
```

应该**完全不修改状态**。

只有整个 batch 合法后再真正提交。

---

# 十一、处理你当前 active segment 的可见性

你现在 `TableStorage` 的语义非常明确：

```text
Append
 ↓
active_segment

Flush
 ↓
sealed / disk segment
 ↓
Scan 可见
```

因此索引也必须保持同样语义。

新插入：

```text
PrimaryKeyIndex:
    立即加入
```

因为下一次 INSERT 必须能够发现重复。

但是：

```text
IndexScan:
    暂时不能看到
```

直到这个 segment 被 Flush。

所以索引条目增加：

```cpp
struct IndexEntry {
    RowLocation location;
    bool visible;
};
```

流程：

```text
Append
 ↓
index entry
visible = false

Flush segment
 ↓
table.meta 更新
 ↓
MarkSegmentVisible(segment_id)
```

这样：

**主键约束看到所有已插入数据；SELECT 只看到和 SeqScan 完全相同的数据。**

不会破坏你现在的可见性语义。

---

# 十二、索引第一版不需要单独持久化

我非常建议第一版不要马上做：

```text
pk.idx
idx_age.idx
```

因为这会马上带来：

```text
index file
segment file
table.meta
```

三者一致性问题。

你这个项目暂时没有 WAL / transaction / recovery，没有必要现在引入。

第一版：

```text
Catalog
    持久化 KeySchema

Segment
    持久化数据

TableStorage::Open()
    ↓
根据 KeySchema
扫描已经持久化的 key columns
    ↓
重新 Build TableIndexManager
```

也就是：

```cpp
TableIndexManager::BuildFromPersistedSegments();
```

启动是 O(N)，但实现非常干净，而且不增加崩溃恢复问题。

以后需要优化启动速度，再加入：

```text
indexes/
    primary.idx
    idx_age.idx
```

即可。

---

# 十三、查询执行：不要改 LogicalPlan，PhysicalPlanner 选择访问路径

你现在：

```text
LogicalScan
    pushed_predicates

↓ PhysicalPlanner

PhysicalSeqScan
```

这套结构很好。

我不建议新建 `LogicalIndexScan`。

因为：

> “使用哪个索引”属于物理执行策略，不属于逻辑语义。

所以改为：

```cpp
class PhysicalPlanner {
public:
    explicit PhysicalPlanner(const Catalog& catalog);
};
```

当遇到：

```cpp
LogicalScan
```

PhysicalPlanner 查看：

```text
table_oid
pushed_predicates
TableSchema::primary_key
TableSchema::secondary_keys
```

例如：

```sql
SELECT name
FROM users
WHERE id = 100;
```

存在：

```text
PRIMARY KEY(id)
```

则生成：

```text
PhysicalPrimaryKeyScan
```

而：

```sql
WHERE age = 20
```

存在：

```text
KEY idx_age(age)
```

则生成：

```text
PhysicalIndexScan
```

否则：

```text
PhysicalSeqScan
```

这与数据库正常的：

```text
Logical Scan
     ↓
Access Path Selection
     ↓
Seq Scan / Index Scan
```

结构一致。

---

# 十四、PhysicalPlan 增加 IndexScan

增加：

```cpp
PhysicalPlan::Type {
    SEQ_SCAN,
    INDEX_SCAN,
    ...
};
```

例如：

```cpp
class PhysicalIndexScan final : public PhysicalPlan {
public:
    TableId GetTableOid() const;

    KeyId GetKeyId() const;

    const std::vector<PlanLiteralValue>&
    GetLookupValues() const;

    const std::vector<ColumnId>&
    GetColumns() const;

    const std::vector<SimplePredicate>&
    GetResidualPredicates() const;

private:
    TableId table_oid_;

    KeyId key_id_;

    std::vector<PlanLiteralValue> lookup_values_;

    std::vector<ColumnId> columns_;

    std::vector<SimplePredicate> residual_predicates_;
};
```

复合键第一版要求：

```text
所有 key columns 都有 EQ
```

才能使用。

例如：

```sql
PRIMARY KEY(a,b)
```

下面可以：

```sql
WHERE a = 1 AND b = 2
```

下面第一版不使用索引：

```sql
WHERE a = 1
```

直接 SeqScan。

---

# 十五、Execution 增加 IndexScanOperator

新增：

```text
src/execution/scan/index_scan.h
src/execution/scan/index_scan.cpp
```

接口：

```cpp
class IndexScanOperator : public Operator {
public:
    bool Next(VectorBatch& output) override;

private:
    std::shared_ptr<TableStorage> table_;

    KeyId key_id_;

    EncodedKey lookup_key_;

    std::vector<RowLocation> locations_;

    size_t current_location_ = 0;

    std::vector<ColumnId> required_columns_;

    std::vector<SimplePredicate> residual_predicates_;
};
```

`ExecutorBuilder`：

```cpp
case PhysicalPlan::Type::INDEX_SCAN:
    return BuildIndexScan(...);
```

---

# 十六、SegmentReader 增加 point/gather read

索引最后得到的是：

```cpp
RowLocation {
    segment_id,
    row_offset
}
```

因此不能再走现在：

```cpp
GetVectorBatch(offset ...)
```

需要增加：

```cpp
bool SegmentReader::GatherRows(
    const std::vector<uint32_t>& row_offsets,
    const std::vector<ColumnId>& columns,
    VectorBatch& output);
```

更推荐 TableStorage 对外：

```cpp
bool TableStorage::ReadRows(
    SegmentId segment_id,
    const std::vector<uint32_t>& row_offsets,
    const std::vector<ColumnId>& columns,
    VectorBatch& output);
```

`IndexScanOperator` 把：

```text
RowLocation
```

按照：

```text
segment_id
```

分组，然后一个 segment 一个 batch 返回。

这和你现在的 columnar Segment 布局很匹配。

---

# 十七、最后形成的结构

最终结构建议是：

```text
                         Catalog
                           │
                     TableSchema
                           │
          ┌────────────────┴────────────────┐
          │                                 │
       Columns                             Keys
                                   ┌────────┴────────┐
                                   │                 │
                             Primary Key       Secondary Key


StorageManager
      │
      ▼
TableStorage
      │
      ├── SegmentBuilder
      ├── SegmentReader
      │
      └── TableIndexManager
              │
              ├── PrimaryKeyIndex
              │       Key -> RowLocation
              │
              └── SecondaryKeyIndex
                      Key -> [RowLocation...]
```

读取：

```text
LogicalScan
     │
     ▼
PhysicalPlanner
     │
     ├── 无合适 Key ───────► PhysicalSeqScan
     │
     └── 有等值 Key ───────► PhysicalIndexScan
                                   │
                                   ▼
                           TableIndexManager
                                   │
                                   ▼
                             RowLocation
                                   │
                                   ▼
                           SegmentReader
```

写入：

```text
INSERT
  │
  ▼
CommandExecutor
  │
  ▼
DataChunk
  │
  ▼
TableStorage::Append
  │
  ├── ValidatePrimaryKey
  │
  ├── AppendInternal
  │
  └── Update Index
```

---

# 十八、我建议你的实际实现顺序

1. **先完成元数据链路**：`Token → Parser → AST → Binder → TableSchema → Catalog V3`，此阶段 CREATE TABLE 能正确保存和重新加载 Key 定义。

2. **实现 PrimaryKeyIndex**：暂时只负责唯一性，不改 SELECT；把检查放进 `TableStorage::Append()`，完成单条、多条 INSERT、跨 INSERT、重启后重新建索引的重复检测。

3. **重构 Append 返回 RowLocation**，为查询索引做准备，并处理 active/persisted 的 visible 状态。

4. **实现 SecondaryKeyIndex 和主键 RowLocation 映射**，此时 PK 不仅能查重，也能定位数据。

5. **增加 `SegmentReader::GatherRows()` + `TableStorage::ReadRows()`**。

6. **增加 PhysicalIndexScan + IndexScanOperator**，让主键和普通 KEY 真正参与查询。

7. **最后修改 PhysicalPlanner 的 Access Path Selection**：完整等值主键优先、完整等值 secondary key 次之，否则继续使用现有 SeqScan。

我认为这是目前 `simple_olap` 最合适的边界。尤其是**不要一上来做 B+Tree 和持久化索引文件**：你现在是 append-only OLAP、没有 UPDATE/DELETE/WAL，`内存 Hash Index + 重启从 Segment rebuild` 能先把“主键约束 + 二级键 + IndexScan”这一整条数据库主线完整打通，同时不破坏你已经稳定下来的 Catalog/Storage/Execution 分层。
