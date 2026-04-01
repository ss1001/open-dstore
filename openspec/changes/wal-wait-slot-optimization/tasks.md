## 1. waiterCount 在 release 模式生效

- [x] 1.1 移除 `WaitPlsnSlots()` 中 `m_plsnWaitSlot[slot].IncreaseWaitCount()` 的 `#ifdef UT` 守卫（src/wal/dstore_wal_logstream.cpp:372-374）
- [x] 1.2 移除 `WaitPlsnSlots()` 中 `m_plsnWaitSlot[slot].DecreaseWaitCount()` 的 `#ifdef UT` 守卫（src/wal/dstore_wal_logstream.cpp:403-405）

## 2. NotifySlotLeaderIfNecessary 优化

- [x] 2.1 移除 `NotifySlotLeaderIfNecessary()` 中 waiterCount == 0 检查的 `#ifdef UT` 守卫，使空 slot 跳过在 release 模式生效（src/wal/dstore_wal_logstream.cpp:353-356）
- [x] 2.2 将 `notify_one()` 改为 `notify_all()`（src/wal/dstore_wal_logstream.cpp:359）

## 3. 验证

- [x] 3.1 在 Docker 容器中编译 release 模式，确认无编译错误
- [x] 3.2 运行 unit test (ut_wal) 确认功能正确性
- [x] 3.3 运行 128 线程 write_only small baseline benchmark（3 次取中位数）
- [x] 3.4 运行 32 线程 write_only small benchmark 确认无退化
- [x] 3.5 运行 32 线程 read_only small benchmark 确认无退化
- [x] 3.6 对比 before/after 结果，输出 Phase 6 结论

## 4. Outcome / Handoff

- [x] 本 change 在存储引擎直连压测中确认有效
- [x] 接入生产链路并叠加 handler / SQL 层后，端到端收益不明显
- [x] 后续 AI 不应优先重复相同的 WAL wait-slot 微优化验证
- [x] 后续应优先做生产式 profiling 与分层耗时定位
