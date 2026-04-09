# Dstore Point Select Optimization Master Plan

> 日期: 2026-04-09
> 目标负载: `sysbench point_select` / 极高并发只读点查
> 目标平台: ARM 架构
> 适用范围: Dstore 单机存储引擎内核热路径优化

---

## 1. 文档目标

本文档用于汇总当前 Dstore 在 `point_select / point query` 场景下的优化方向，并给出统一的落地优先级。

与已有文档的关系：

- 基础分析报告: [`/.ai/summaries/point_select_optimization_report.md`](/Users/shesong/work/code/dstore/.ai/summaries/point_select_optimization_report.md)
- 扩展分析报告: [`/docs/optimizations/point_select_extended_optimization.md`](/Users/shesong/work/code/dstore/docs/optimizations/point_select_extended_optimization.md)
- 本文档: 在前两份文档基础上，结合本地 `dstore` 代码热路径复核、额外前沿方案调研，形成统一路线图

本文档重点解决三件事：

1. 统一汇总已有优化项与新增优化项
2. 为每个优化项明确标注来源
3. 按落地优先级给出建议实施顺序

---

## 2. 来源说明

本文档中的每个优化项都会明确标记来源，来源缩写如下：

- `R1`: 基础优化报告  
  参考 [`/.ai/summaries/point_select_optimization_report.md`](/Users/shesong/work/code/dstore/.ai/summaries/point_select_optimization_report.md)
- `R2`: 扩展优化报告  
  参考 [`/docs/optimizations/point_select_extended_optimization.md`](/Users/shesong/work/code/dstore/docs/optimizations/point_select_extended_optimization.md)
- `R3`: 本地代码复核分析  
  基于当前仓库真实代码路径分析得到
- `R4`: 额外前沿调研  
  包括 2024-2026 学术界/工业界公开材料，用于补充中长期方向

说明：

- `R1`、`R2` 中已有内容，本文件只做统一归档与优先级重排
- `R3` 是本轮最重要的新增部分，因为它直接结合了当前 `dstore` 实现细节
- `R4` 更适合作为中长期演进方向，不建议优先于高收益低风险的内核内改造

---

## 3. 当前 Point Select 热路径复核

结合现有代码，当前典型热路径可概括为：

```text
SnapshotCsn
  -> Index Scan 初始化
  -> Root 获取
  -> Internal Page 逐层下降
  -> Leaf 命中得到 heap ctid
  -> Heap Fetch
  -> Visibility Check
  -> Tuple 物化
  -> Scan Handler 清理
```

关键代码位置：

- 点查入口: [`/tests/utilities/src/table_handler.cpp:440`](/Users/shesong/work/code/dstore/tests/utilities/src/table_handler.cpp#L440)
- B-Tree 查找: [`/src/index/dstore_btree_scan.cpp:2241`](/Users/shesong/work/code/dstore/src/index/dstore_btree_scan.cpp#L2241)
- Root / Meta cache: [`/src/index/dstore_btree.cpp:614`](/Users/shesong/work/code/dstore/src/index/dstore_btree.cpp#L614)
- Buffer content lock: [`/src/buffer/dstore_buf_mgr.cpp:2122`](/Users/shesong/work/code/dstore/src/buffer/dstore_buf_mgr.cpp#L2122)
- Heap Fetch: [`/src/heap/dstore_heap_scan.cpp:694`](/Users/shesong/work/code/dstore/src/heap/dstore_heap_scan.cpp#L694)
- Heap visible tuple: [`/src/heap/dstore_heap_scan.cpp:949`](/Users/shesong/work/code/dstore/src/heap/dstore_heap_scan.cpp#L949)
- Snapshot CSN 获取: [`/src/transaction/dstore_transaction.cpp:1513`](/Users/shesong/work/code/dstore/src/transaction/dstore_transaction.cpp#L1513)
- CSN manager: [`/src/transaction/dstore_csn_mgr.cpp:134`](/Users/shesong/work/code/dstore/src/transaction/dstore_csn_mgr.cpp#L134)

当前的主要瓶颈类型：

1. 共享读热点  
   例如 root/internal page 的共享锁、snapshot/CSN 全局状态读取、meta cache 验证
2. 原子操作与缓存一致性流量  
   例如 pin/unpin、buffer 状态访问、锁状态更新
3. 泛型扫描框架税  
   单键唯一点查仍走通用 `ScanBegin/ReScan/ScanNext/ScanEnd`
4. Heap 路径过重  
   包括可见性判断、tuple copy、handler 生命周期开销
5. 指令前端与 cache 局部性问题  
   包括 `BufferDesc` 冷热混布、比较函数泛型化、分支路径较长

---

## 4. 优先级分层

优先级分层标准如下：

- `P0`: 强烈建议优先落地，命中热路径核心矛盾，收益大且可工程化推进
- `P1`: 建议紧随 P0 落地，收益高，但通常依赖前置整理或需要更多验证
- `P2`: 中期优化，收益可能不错，但工程面更广或需要 profiling 进一步确认
- `P3`: 中长期探索项，偏架构演进或前沿方案

---

## 5. 范围边界与排序原则

为了避免把“引擎内核优化”和“系统/压测侧调优”混为一谈，本文档采用以下边界：

### 5.1 纳入主排序的内容

以下内容进入本文档主排序：

- `dstore` 内核热路径直接相关的代码改造
- 会稳定影响 `point_select` 单次延迟或高并发吞吐的结构性优化
- 与 ARM 平台直接相关的编译、页表、原子、向量化优化

### 5.2 不纳入主排序但仍有价值的内容

以下内容不作为本文档主排序项，仅作为旁路收益项参考：

- 压测方式优化  
  例如 Prepared Statements、sysbench 参数调整、客户端连接模型
- 系统部署调优  
  例如 RSS/RPS 绑核、TCP backlog、socket 参数
- 代理层缓存  
  例如 Proxy 微缓存、结果缓存
- 面向写事务或混合负载的优化  
  例如 `io_uring + SQPOLL` 对 WAL/刷盘路径的收益

原因很简单：这些项可能对 benchmark 分数有帮助，但它们并不直接解决当前 `dstore point_select` 内核热路径中的共享热点、泛型框架税和 heap 访问开销。

### 5.3 当前排序原则

本文档中的优先级排序遵循以下原则：

1. 先消除热路径上的共享读/写热点
2. 再去掉泛型 scan 框架税
3. 再把 heap fetch 和 tuple materialization 特化为 point_get 快路径
4. 最后再考虑编译器、内存分配器、系统页表、前沿架构演进

---

## 6. PointGet 专用目标路径

对 `sysbench point_select`，建议把最终目标路径明确为一条“点查专用链路”，而不是继续叠加在通用 scan 框架上：

```text
Get Stable Snapshot CSN
  -> PointGetUnique(index key)
  -> Read Root Descriptor
  -> Lock-free / optimistic internal traversal
  -> Get heap ctid
  -> Fast heap page pin
  -> Visible-fast-check
  -> Borrowed tuple view
  -> Delayed materialization if needed
```

这条路径对应的优化分工如下：

- `Snapshot CSN 快路径化`: 减少每 query 的共享状态读取
- `PointGet 专用快速路径`: 去掉 `ScanBegin/ReScan/ScanNext/ScanEnd`
- `OLC + TL Read Cache`: 降低 root/internal 级别锁与 pin/unpin 成本
- `Heap 可见性快路径`: 避免走完整 MVCC 判断分支链
- `Heap Tuple 零拷贝`: 避免每次 fetch 都复制完整 tuple

如果后续评估某个优化项不能推动这条专用目标路径变短，那它的优先级就不应该排在前面。

---

## 7. ARM 平台专项关注点

由于本文档目标平台是 ARM，以下内容应显式作为实现时的关注重点，而不是沿用 x86 默认思路：

### 7.1 原子与锁

- `LSE atomics` 应作为默认编译与部署前提验证项
- 所有依赖 CAS 热点的结构都要结合 ARM 下的 cacheline bouncing 再评估一次
- 在 ARM 上，减少共享原子操作的价值通常高于“把单个原子做快一点”

### 7.2 页内比较与向量化

- 不应只写成 “SIMD/AVX2/AVX-512”
- 对 `unique + int4 + non-null` 的典型点查，需要明确补充：
  - `NEON` 专用实现
  - `SVE` 可选实现
  - 当 gather 不友好时的顺序装载与结构布局策略

### 7.3 页表与 TLB

- ARM 的 HugePage / Contiguous PTE / mTHP 值得单独验证
- Buffer Pool、Index Metadata、只读元数据区应尽量分离映射策略
- 对上层索引和热点元数据区，TLB miss 往往比普通顺序内存带宽更早成为问题

### 7.4 文档结论

因此，凡是涉及 `SIMD`、`atomics`、`hugepage` 的方案，在本项目中都应优先写出 ARM 落地版本，而不是只列通用概念。

---

## 8. 优化项总表

| 优先级 | 优化项 | 主要来源 | 是否已在旧文档中出现 | 与 dstore 当前代码直接相关 |
|---|---|---|---|---|
| P0 | B-Tree 读路径 OLC 化 | R1 + R3 | 是 | 是 |
| P0 | Thread-local Read Cache / 私有 pin 缓存 | R1 + R3 | 是 | 是 |
| P0 | Snapshot CSN 快路径化 | R3 | 否 | 是 |
| P0 | PointGet 专用快速路径 | R3 | 否 | 是 |
| P0 | BufferDesc 热冷字段分离 | R1 + R3 | 是 | 是 |
| P1 | Heap Tuple 零拷贝 / 延迟物化 | R3 | 否 | 是 |
| P1 | Heap 可见性极简快路径 | R2 + R3 | 部分相关 | 是 |
| P1 | Unique Int4 Key 专用比较内核 | R3 | 否 | 是 |
| P1 | Root / Meta Cache Epoch 发布式读取 | R3 | 否 | 是 |
| P1 | Pointer Swizzling | R2 + R3 | 是 | 是 |
| P2 | 编译器与二进制布局优化 | R2 | 是 | 间接相关 |
| P2 | 现代内存分配器与对象池 | R2 | 是 | 间接相关 |
| P2 | Buffer Pool 分区化 | R2 | 是 | 是 |
| P2 | ARM HugePage / TLB 专项优化 | R4 | 否 | 间接相关 |
| P2 | Mini-page / Hot Fragment Cache | R4 | 否 | 是 |
| P3 | EPVS / Epoch-protected Metadata | R4 | 否 | 是 |
| P3 | OptiQL 式高争用乐观锁 | R4 | 否 | 是 |
| P3 | Learned Upper Directory / VEGA | R4 | 否 | 是 |
| P3 | SmartNIC / DPU 卸载点查 | R4 | 否 | 否 |
| P3 | Zero-sided RDMA / Switch Assisted Fetch | R4 | 否 | 否 |

---

## 9. 各优化项详细说明

### P0-1. B-Tree 读路径 OLC 化

- 来源:
  - `R1` 基础优化报告 A4
  - `R3` 本地代码复核
- 相关代码:
  - [`/src/index/dstore_btree_scan.cpp:2241`](/Users/shesong/work/code/dstore/src/index/dstore_btree_scan.cpp#L2241)
  - [`/src/index/dstore_btree.cpp:571`](/Users/shesong/work/code/dstore/src/index/dstore_btree.cpp#L571)
- 当前问题:
  - `SearchBtreeFromInternalPage()` 在 root 和 internal page 上仍然依赖 `LW_SHARED`
  - 并发读者会集中竞争热点页的锁状态
- 建议改造:
  - 在页头引入版本号或变体校验信息
  - internal page 使用无锁读取 + version validate
  - 多次失败再 fallback 到悲观路径
- 预期收益:
  - 显著降低 root/internal page 锁竞争
  - 在高并发只读场景通常是第一梯队收益项
- 风险:
  - 需要小心页分裂、右移、unlink 等并发一致性

### P0-2. Thread-local Read Cache / 私有 pin 缓存

- 来源:
  - `R1` 基础优化报告 A8
  - `R3` 本地代码复核
- 相关代码:
  - [`/src/buffer/dstore_buf_mgr.cpp:860`](/Users/shesong/work/code/dstore/src/buffer/dstore_buf_mgr.cpp#L860)
  - [`/src/index/dstore_btree.cpp:571`](/Users/shesong/work/code/dstore/src/index/dstore_btree.cpp#L571)
- 当前问题:
  - 点查下降过程是典型的“pin 新页、release 旧页”
  - 线程私有 refcount 的收益还没有完全发挥出来
- 建议改造:
  - 对 root / upper internal / 热 leaf 建立 thread-local read cache
  - 支持重复读取同页时完全绕过共享原子计数
- 预期收益:
  - 直接减少 pin/unpin 引发的共享 cacheline traffic
- 风险:
  - 需要配合 buffer invalidation / version check

### P0-3. Snapshot CSN 快路径化

- 来源:
  - `R3` 本地代码复核
- 相关代码:
  - [`/src/transaction/dstore_transaction.cpp:1513`](/Users/shesong/work/code/dstore/src/transaction/dstore_transaction.cpp#L1513)
  - [`/src/transaction/dstore_csn_mgr.cpp:134`](/Users/shesong/work/code/dstore/src/transaction/dstore_csn_mgr.cpp#L134)
- 当前问题:
  - 只读 query 获取 snapshot 时仍读取全局 `m_nextCsn`
  - 高并发短事务下，这是一个固定的共享状态读热点
- 建议改造:
  - 增加 `published_visible_csn` 或同类只读发布变量
  - `point_select` 读取稳定可见 CSN，而不是直接碰 `m_nextCsn`
  - cursor / flashback / 特殊事务保持原语义
- 预期收益:
  - 降低全局 CSN 状态上的 cacheline 争用
  - 对极高 QPS 点查是持续收益
- 风险:
  - 必须严格验证 RC/MVCC 语义

### P0-4. PointGet 专用快速路径

- 来源:
  - `R3` 本地代码复核
- 相关代码:
  - [`/tests/utilities/src/table_handler.cpp:447`](/Users/shesong/work/code/dstore/tests/utilities/src/table_handler.cpp#L447)
  - [`/src/index/dstore_index_handler.cpp:49`](/Users/shesong/work/code/dstore/src/index/dstore_index_handler.cpp#L49)
  - [`/src/heap/dstore_heap_interface.cpp:335`](/Users/shesong/work/code/dstore/src/heap/dstore_heap_interface.cpp#L335)
- 当前问题:
  - 单键唯一点查仍走完整 scan handler 生命周期
  - 每次 query 都有对象初始化、scan key copy、状态切换
- 建议改造:
  - 新增 `PointGetUnique` 风格接口
  - 直接执行“唯一键查索引 -> 得 ctid -> 快速读 heap”
  - 尽量复用 thread-local handler 或专用轻量上下文
- 预期收益:
  - 降低框架层固定税
  - 很适合 sysbench point_select
- 风险:
  - 需要维护一个与通用 scan 逻辑并存的专用分支

### P0-5. BufferDesc 热冷字段分离

- 来源:
  - `R1` 基础优化报告 A3
  - `R3` 本地代码复核
- 相关代码:
  - [`/include/buffer/dstore_buf.h`](/Users/shesong/work/code/dstore/include/buffer/dstore_buf.h)
- 当前问题:
  - `BufferDesc` 字段过多，冷热信息混布
  - 热路径读取 state、lock、tag 时容易拖入冷字段 cacheline
- 建议改造:
  - 将 `state`、`contentLwLock`、`bufTag`、`bufBlock` 等重排到首个 cacheline
  - dirty queue / recovery / 冷元数据下沉
- 预期收益:
  - 减少 buffer 相关 cache miss
- 风险:
  - 低风险，但需要检查偏移依赖和序列化假设

### P1-1. Heap Tuple 零拷贝 / 延迟物化

- 来源:
  - `R3` 本地代码复核
- 相关代码:
  - [`/src/heap/dstore_heap_scan.cpp:694`](/Users/shesong/work/code/dstore/src/heap/dstore_heap_scan.cpp#L694)
  - [`/src/heap/dstore_heap_scan.cpp:920`](/Users/shesong/work/code/dstore/src/heap/dstore_heap_scan.cpp#L920)
- 当前问题:
  - 当前 `FetchTuple()` 常常会复制 tuple
  - 对“取到即返回”的点查是额外内存带宽消耗
- 建议改造:
  - 引入 borrowed tuple view 或 tuple ref
  - 仅在必要时 materialize
- 预期收益:
  - 对 value 较长、读后即返回的场景尤其明显
- 风险:
  - 生命周期管理复杂度上升

### P1-2. Heap 可见性极简快路径

- 来源:
  - `R2` 扩展报告中的无锁化可见性方向
  - `R3` 本地代码复核
- 相关代码:
  - [`/src/heap/dstore_heap_scan.cpp:949`](/Users/shesong/work/code/dstore/src/heap/dstore_heap_scan.cpp#L949)
  - [`/src/transaction/dstore_transaction.cpp:1313`](/Users/shesong/work/code/dstore/src/transaction/dstore_transaction.cpp#L1313)
- 当前问题:
  - 只读点查大多数时候并不需要完整通用 MVCC 判断路径
- 建议改造:
  - 引入 page-level visible hint
  - 对 frozen / committed / 无 pending 依赖 tuple 做快判
  - 与 `Borrowed Tuple View` 组合，形成 `visible-fast-check -> tuple view -> delayed materialize` 的连续快路径
- 预期收益:
  - 降低 heap fetch 阶段的分支和状态查询开销
- 风险:
  - 可见性 hint 必须保守正确

### P1-3. Unique Int4 Key 专用比较内核

- 来源:
  - `R3` 本地代码复核
- 相关代码:
  - [`/src/index/dstore_btree.cpp:956`](/Users/shesong/work/code/dstore/src/index/dstore_btree.cpp#L956)
  - [`/src/index/dstore_btree.cpp:1018`](/Users/shesong/work/code/dstore/src/index/dstore_btree.cpp#L1018)
- 当前问题:
  - 已有 `CompareNIntKeyWithoutNulls()`，但仍嵌在通用比较路径里
- 建议改造:
  - 对 `unique + single int4 + non-null` 建立专用 fast path
  - 避免走通用 `GetAttr()` 和通用函数比较
  - ARM 平台上优先实现 `NEON` 版本，`SVE` 作为增强版本
- 预期收益:
  - 降低 branch 与函数调用成本
- 风险:
  - 低风险，适合专用路径落地

### P1-4. Root / Meta Cache Epoch 发布式读取

- 来源:
  - `R3` 本地代码复核
- 相关代码:
  - [`/src/index/dstore_btree.cpp:614`](/Users/shesong/work/code/dstore/src/index/dstore_btree.cpp#L614)
- 当前问题:
  - root cache 当前仍偏保守，每次都做较重验证
- 建议改造:
  - 发布 `root descriptor + generation`
  - 常态下 reader 只读 descriptor，失败再 fallback
- 预期收益:
  - 把 root 获取继续只读化
- 风险:
  - 需要处理 root split / stale cache / invalidation

### P1-5. Pointer Swizzling

- 来源:
  - `R2` 扩展优化报告
  - `R3` 本地代码复核支持
- 当前问题:
  - 父到子的跳转仍依赖 pageId -> buffer lookup
- 建议改造:
  - 对 root 和上层 internal page downlink 做内存指针驻留
- 预期收益:
  - 减少 lookup 与间接跳转
- 风险:
  - 需要可靠的失效与回收策略

### P2-1. 编译器与二进制布局优化

- 来源:
  - `R2`
- 内容:
  - `ThinLTO`
  - `AutoFDO`
  - `Propeller / BOLT`
- 作用:
  - 改善 I-cache、分支布局与跨模块内联
- 建议:
  - 在热路径结构基本稳定后做

### P2-2. 现代分配器与对象池

- 来源:
  - `R2`
- 作用:
  - 降低 handler、tuple、临时对象分配抖动
- 建议:
  - 与 PointGet 快路径配套推进

### P2-3. Buffer Pool 分区化

- 来源:
  - `R2`
- 作用:
  - 降低 buftable / LRU 级别竞争
- 建议:
  - 先确认 profiling 确实表明全局结构争用明显

### P2-4. ARM HugePage / TLB 专项优化

- 来源:
  - `R4`
  - 参考资料:
    - [Transparent Hugepage Support](https://docs.kernel.org/admin-guide/mm/transhuge.html)
    - [HugeTLBpage on ARM64](https://docs.kernel.org/6.0/arm64/hugetlbpage.html)
- 作用:
  - 降低 buffer pool / index metadata 区域的 TLB miss
- 建议:
  - 分离 mmap arena，对热点元数据区做更激进 hugepage 策略
  - 优先验证 root/internal/index metadata 区域，而不是对整个 buffer pool 一刀切开启
  - 在 ARM 上单独记录 `dTLB-load-misses`、page walk、minor fault 变化

### P2-5. Mini-page / Hot Fragment Cache

- 来源:
  - `R4`
  - 参考资料:
    - [Bf-Tree, PVLDB 2024](https://badrish.net/papers/bftree-vldb2024.pdf)
- 作用:
  - 比 AHI 更细粒度地缓存热记录或热叶片段
- 建议:
  - 作为中期 redesign 方向

### P3-1. EPVS / Epoch-protected Metadata

- 来源:
  - `R4`
  - 参考资料:
    - [VLDB Journal 2024 EPVS](https://link.springer.com/article/10.1007/s00778-024-00859-8)
- 作用:
  - 把 reader 的共享写动作进一步挪出快路径
- 建议:
  - 优先应用在 snapshot/metadata/root publication 等元数据结构

### P3-2. OptiQL 式高争用乐观锁

- 来源:
  - `R4`
  - 参考资料:
    - [SIGMOD 2024 OptiQL](https://2024.sigmod.org/toc.html)
- 作用:
  - 提升热点锁对象在极端并发下的鲁棒性
- 建议:
  - 可用于 meta slot、hash bucket、root header 等热点对象

### P3-3. Learned Upper Directory / VEGA

- 来源:
  - `R4`
  - 参考资料:
    - [SIGMOD 2025 VEGA](https://2025.sigmod.org/toc-3-1.html)
- 作用:
  - 将上层索引目录 learned 化，减少层级查找成本
- 建议:
  - 仅建议做上层目录，不建议直接替换叶层验证逻辑

### P3-4. SmartNIC / DPU 卸载点查

- 来源:
  - `R4`
  - 参考资料:
    - [Employ SmartNICs' Data Path Accelerators for Ordered Key-Value Stores, arXiv 2026](https://arxiv.org/abs/2601.06231)
- 作用:
  - 把上层目录或热点 KV 缓存前移到 NIC/DPU
- 建议:
  - 仅适合中长期架构演进

### P3-5. Zero-sided RDMA / Switch Assisted Fetch

- 来源:
  - `R4`
  - 参考资料:
    - [Zero-sided RDMA, SIGMOD/PACMMOD 2024](https://www.dfki.de/web/forschung/projekte-publikationen/publikation/16460)
- 作用:
  - 适合存算分离或远端页缓存场景
- 建议:
  - 不建议优先于单机热路径优化

---

## 10. 旁路收益项与非主线项

以下项目可能对 benchmark 或特定部署有效，但不进入主线优先级排序：

- Prepared Statements / 二进制协议
- RSS/RPS / 中断绑核 / TCP 参数
- Proxy 微缓存
- `io_uring + SQPOLL` 对 WAL/刷盘路径的优化

建议把这些项目独立放在 benchmark 或部署手册里，避免与内核热路径优化混排。

---

## 11. 建议实施顺序

综合考虑收益、风险与代码现状，建议按以下顺序推进：

1. B-Tree 读路径 OLC 化
2. Thread-local Read Cache / 私有 pin 缓存
3. Snapshot CSN 快路径化
4. PointGet 专用快速路径
5. BufferDesc 热冷字段分离
6. Heap Tuple 零拷贝 / 延迟物化
7. Heap 可见性极简快路径
8. Unique Int4 Key 专用比较内核
9. Root / Meta Cache Epoch 发布式读取
10. Pointer Swizzling
11. 编译器与二进制布局优化
12. 现代分配器与对象池
13. Buffer Pool 分区化
14. ARM HugePage / TLB 专项优化
15. Mini-page / Hot Fragment Cache
16. EPVS / OptiQL / Learned Upper Directory
17. SmartNIC / DPU / Zero-sided RDMA

---

## 12. 建议的首批组合

如果只做第一波、并追求最现实的吞吐提升，建议优先组合以下 5 项：

1. B-Tree 读路径 OLC 化
2. Thread-local Read Cache / 私有 pin 缓存
3. Snapshot CSN 快路径化
4. PointGet 专用快速路径
5. Heap Tuple 零拷贝 / 延迟物化

这 5 项合起来分别命中：

- 锁竞争
- 原子 pin/unpin 流量
- snapshot 全局热点
- 框架层固定税
- tuple copy 成本

这是目前最有希望在 `sysbench point_select` 上获得明显复合收益的组合。

---

## 13. 验证指标建议

首批优化落地时，建议至少同时跟踪以下指标，而不要只看 QPS：

- `QPS / P50 / P95 / P99`
- `cycles / instructions / IPC`
- `branch-misses`
- `L1-dcache-load-misses`
- `LLC-load-misses`
- `dTLB-load-misses`
- `atomic` 相关热点函数占比
- root/internal page 锁争用与重试次数
- `FetchTuple` / `VisibilityCheck` / `ScanBegin` 相关函数占比

其中：

- 做 `OLC` 时重点观察锁争用、重试率、LLC miss
- 做 `Snapshot CSN` 时重点观察热点 cacheline 与原子/共享读开销
- 做 `PointGet` 时重点观察 `ScanBegin/ReScan/ScanNext` 等框架函数占比
- 做 `Heap Tuple 零拷贝` 时重点观察 `memcpy`、内存带宽、cache miss
- 做 `ARM HugePage` 时重点观察 `dTLB-load-misses` 与 page walk

---

## 14. 总结

从当前 `dstore` 代码实现出发，最优先的方向并不是继续堆新的系统级黑科技，而是先把已有热点路径彻底“点查专用化、只读化、轻量化”。

当前最应该优先处理的核心问题是：

1. Root/internal page 共享锁竞争
2. pin/unpin 与 snapshot 相关共享状态热点
3. 单键点查仍走泛型 scan 框架
4. heap fetch 和 tuple materialization 对只读点查仍偏重

在这些问题没有被充分榨干之前，更重型的 SmartNIC、DPU、learned index、switch-assisted fetch 等方案不应作为主线优先项。
