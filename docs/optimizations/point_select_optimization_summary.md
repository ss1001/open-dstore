# Dstore Point Select Optimization Summary

> 日期: 2026-04-09
> 场景: `sysbench point_select`
> 平台: ARM
> 用途: 对外分享 / 评审汇报 / 阶段总结

---

## 1. 结论摘要

针对 Dstore 的高并发只读点查，当前最值得优先投入的方向，不是继续堆叠系统层或网络层黑科技，而是先把现有热路径彻底“点查专用化、只读化、轻量化”。

当前最关键的 4 类瓶颈是：

1. Root / internal page 上的共享锁与共享状态热点
2. pin/unpin、snapshot/CSN 读取带来的共享 cacheline 流量
3. 单键点查仍走通用 scan 框架，存在明显框架税
4. heap fetch、可见性判断、tuple 复制对只读点查仍偏重

---

## 2. 文档来源

本摘要整合自以下三部分：

- 基础报告: [`/.ai/summaries/point_select_optimization_report.md`](/Users/shesong/work/code/dstore/.ai/summaries/point_select_optimization_report.md)
- 扩展报告: [`/docs/optimizations/point_select_extended_optimization.md`](/Users/shesong/work/code/dstore/docs/optimizations/point_select_extended_optimization.md)
- 总方案文档: [`/docs/optimizations/point_select_optimization_master_plan.md`](/Users/shesong/work/code/dstore/docs/optimizations/point_select_optimization_master_plan.md)

其中：

- 基础报告提供了 `Thread-local Read Cache`、`BufferDesc 热冷分离` 等主线方向
- 扩展报告补充了 `Pointer Swizzling`、`Buffer Pool 分区化`、编译器与分配器方向
- 总方案文档结合本地代码进一步补出了 `Snapshot CSN 快路径`、`PointGet 专用路径`、`Heap fast path` 等更贴近当前 dstore 的高优先项

---

## 3. 推荐优先级

### P0: 第一优先级，建议先做

1. `Thread-local Read Cache / 私有 pin 缓存`
2. `Snapshot CSN 快路径化`
3. `PointGet 专用快速路径`
4. `BufferDesc 热冷字段分离`

### P1: 第二优先级，建议紧接着做

5. `Heap Tuple 零拷贝 / 延迟物化`
6. `Heap 可见性极简快路径`
7. `Unique Int4 Key 专用比较内核`
8. `Root / Meta Cache Epoch 发布式读取`
9. `Pointer Swizzling`

### P2: 中期优化

10. `编译器与二进制布局优化`
11. `现代内存分配器与对象池`
12. `Buffer Pool 分区化`
13. `ARM HugePage / TLB 专项优化`
14. `Mini-page / Hot Fragment Cache`

### P3: 中长期探索

15. `EPVS / Epoch-protected Metadata`
16. `Learned Upper Directory / VEGA`
17. `SmartNIC / DPU 卸载点查`
18. `Zero-sided RDMA / Switch-assisted Fetch`

---

## 4. 最值得优先落地的 4 项

### 4.1 Thread-local Read Cache

- 来源: 基础报告 + 本地代码复核
- 作用: 减少 pin/unpin 原子操作与 buffer 共享状态流量
- 价值: 直接减少高并发点查中的共享状态写入

### 4.2 Snapshot CSN 快路径

- 来源: 本地代码复核
- 作用: 避免每个只读 query 都去触碰全局 `m_nextCsn`
- 价值: 对高并发短只读事务尤其关键

### 4.3 PointGet 专用快速路径

- 来源: 本地代码复核
- 作用: 绕过 `ScanBegin/ReScan/ScanNext/ScanEnd` 泛型框架
- 价值: 很适合 sysbench 主键点查模式

### 4.4 Heap fast path

- 来源: 本地代码复核 + 扩展报告
- 包含:
  - `Heap Tuple 零拷贝 / 延迟物化`
  - `Heap 可见性极简快路径`
- 价值: 这是目前文档里最容易被低估的一段热路径

---

## 5. ARM 相关补充结论

这轮总结有一个明确结论：对 ARM 平台，优化重点不能只停留在“开启 LSE”。

还应重点关注：

1. `unique + int4 + non-null` 的专用比较内核，应优先补 `NEON` / `SVE` 实现
2. HugePage / mTHP / TLB 行为应单独验证
3. ARM 下共享原子操作的成本很高，减少共享状态写入通常比优化单条原子指令更重要

---

## 6. 不建议和主线混排的项

以下项目有价值，但不建议与内核热路径主优化混排：

- Prepared Statements
- RSS/RPS / TCP 参数 / IRQ 绑核
- Proxy 微缓存
- `io_uring + SQPOLL` 对 WAL/刷盘路径的优化

原因是这些项更偏压测、部署或混合负载收益，不直接解决当前 point_select 内核热路径矛盾。

---

## 7. 建议分享时的主线表述

可以用一句话概括这份总结：

> Dstore 的 point_select 下一阶段优化重点，不是继续增加外围“黑科技”，而是先把读路径改造成一条面向 ARM 的、无共享热点、低原子操作、低框架税的专用点查快路径。

如果需要一句更偏工程落地的版本：

> 优先做 TL Read Cache、Snapshot CSN 快路径、PointGet 专用接口、Heap fast path，这四项最有希望在当前代码基础上带来可验证的复合收益。

