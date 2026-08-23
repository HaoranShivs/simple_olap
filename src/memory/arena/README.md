# memory/arena

Arena 内存分配器：面向执行期的批量内存管理。

## 职责

- 提供"批量分配、整体释放"的内存池：
  - `alloc(size, align) -> ptr`：从 arena 中顺序分配
  - `reset()`：一次性释放全部内存
- 为执行引擎的临时对象（Batch、哈希表桶、中间结果）提供低开销分配
- 支持按查询/按算子划分 arena 作用域

## 设计要点

- 分配即指针移动（bump allocation），无逐块释放开销
- 大块分配直接走系统分配器，小块从预分配 slab 中切分
- 查询结束即 reset，避免内存碎片与泄漏
- 可统计各 arena 用量，用于内存限制（memory limit）控制