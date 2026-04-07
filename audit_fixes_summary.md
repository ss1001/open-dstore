# Dstore 代码审计问题修复总结报告 (2026-04-07)

本报告总结了针对 `dstore_final_audit.csv` 审计清单中发现的问题所执行的修复工作。修复主要集中在 `PerfCounter` 模块的并发安全与健壮性，以及 `Common_Memory` 模块的关键逻辑。

## 1. PerfCounter (性能计数器) 模块修复

### 线程安全与生命周期管理 (PERF-02, PERF-03, PERF-04)
*   **修复内容：** 
    *   为 `m_taskId` 和 `m_bgThread` 指针数组添加了默认初始化 `{}`，防止进程启动时引用垃圾内存（PERF-04）。
    *   在 `CreateTask` 中使用 `try-catch` 包裹线程创建过程。若创建失败，将原子性地回退任务计数 `m_taskCnt`，并清理已分配的资源（PERF-02, PERF-03）。
    *   在销毁路径 `CancelAllTask` 中增加了空指针和 `joinable()` 检查，彻底消除退出时的并发崩溃风险。

### 内存与初始化安全 (PERF-05, PERF-09, PERF-17)
*   **修复内容：**
    *   显式初始化 `m_perfCatalogMemCtx` 为 `nullptr`，防止构造失败时的非法内存释放（PERF-05）。
    *   补齐了 `RangePartitionStat` 析构函数中对动态数组 `m_counters` 的释放逻辑，解决了长期运行下的内存泄漏问题（PERF-09）。
    *   将 Release 模式下无效的 `ASSERT(m_perfUnits != nullptr)` 替换为运行时 `if` 检查，确保系统在未初始化状态下的安全性（PERF-17）。

### 算法与数据安全 (PERF-08, PERF-15, PERF-10, PERF-18)
*   **修复内容：**
    *   **随机数重构：** 移除了非线程安全的全局 `srand/rand`，引入了基于 `thread_local std::mt19937` 的现代随机数生成引擎，消除了 SkipList 在高并发下的数据竞争并优化了拓扑分布（PERF-08）。
    *   **整型溢出防御：** 调整了 `RangePartitionStat` 初始化中的除法顺序，确保在计算前完成边界校验，防止下溢和除零错误（PERF-15）。
    *   **类型安全：** 修正了 `int64` 的打印格式宏，解决了 `sprintf_s` 的栈读取越界（PERF-10）。
    *   **边界保护：** 修正了 `snprintf_s` 的 `maxCount` 参数，严格遵循 Secure C API 规范（PERF-18）。

### 日志与控制流清理 (PERF-16, PERF-19, PERF-28)
*   **修复内容：**
    *   删除了注册流程中永远不会触发的死代码检查（PERF-16）。
    *   移除了加锁/解锁正常路径上的 `ERROR` 级别日志，消除了高并发下的 I/O 阻塞和日志污染（PERF-19）。
    *   移除了 `DSTORE_PANIC` 后冗余的 `return` 语句，保持引擎异常处理风格的一致性（PERF-28）。

## 2. Common_Memory (通用内存) 模块修复 (同步修复)

### 逻辑与安全增强 (CM-04, CM-08)
*   **修复内容：**
    *   **逻辑纠正：** 修正了 `MCXT_ALLOC_NO_OOM` 标志的逻辑反转问题。现在只有在未设置“不报错”标志时，内存分配失败才会记录 Error 日志（CM-04）。
    *   **拷贝安全：** 在 `DstoreMemcpySafelyForHugeSize` 中增加了对拷贝长度的断言防御，进一步加固了大内存拷贝的安全性（CM-08）。

---
**注：** 以上所有变更已提交至远程分支 `ss1001/fix-perfcounter-audit`。
