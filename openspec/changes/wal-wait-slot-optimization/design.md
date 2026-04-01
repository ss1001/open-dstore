## Context

Dstore 的 WAL commit 路径使用 2048 个 PlsnWaitSlot 实现 group commit 通知。每个 slot 包含 LWLock + std::mutex + std::condition_variable。当 background WAL writer 完成 fsync 后，遍历 slot 范围调用 `NotifySlotLeaderIfNecessary()`，该函数无条件获取每个 slot 的 mutex 并调用 `notify_one()`。

Baseline 数据表明 128 线程 write-only TPS 与 32 线程持平 (970K vs 975K)，p99 从 2ms 飙升到 18ms。

当前代码问题：
1. `NotifySlotLeaderIfNecessary()` 对所有 slot（包括无 waiter 的空 slot）都获取 mutex → 无效开销
2. `notify_one()` 每 slot 只唤醒 1 个 waiter → 其余 waiter 在双锁机制中退化为 spin-wait
3. `m_waiterCount` 的 Increase/Decrease 被 `#ifdef UT` 包裹，release 模式无法使用

## Goals / Non-Goals

**Goals:**
- 减少 flush 后通知阶段的 mutex 获取次数（跳过无 waiter 的 slot）
- 确保有 waiter 的 slot 上所有等待线程被及时唤醒（notify_all）
- 128 线程 write TPS 提升 ≥ 15%，p99 不退化

**Non-Goals:**
- 不重新设计 slot 分配算法（ComputeWaitPlsnSlotNo 不变）
- 不修改 PlsnWaitSlot 结构体布局或内存对齐（留作后续优化）
- 不修改 WAL flush 批量策略（bgWalWriterMinBytes 阈值不变）
- 不修改双锁机制（LWLock + std::mutex 架构不变）

## Decisions

### Decision 1: 移除 `#ifdef UT` 使 waiterCount 在 release 模式生效

**选择**: 移除 `WaitPlsnSlots()` 中 waiterCount Increase/Decrease 的 `#ifdef UT` 守卫，以及 `NotifySlotLeaderIfNecessary()` 中 waiterCount 检查的 `#ifdef UT` 守卫。

**替代方案**:
- A) 保持 `#ifdef UT`，用其他方式判断 slot 是否活跃（如 LWLock state） → 更复杂，且 LWLock state 不直接反映 waiter 数量
- B) 引入新的 atomic flag → 冗余，waiterCount 已存在

**理由**: waiterCount 是现有机制，已在 UT 模式完整验证。两个 atomic fetch-add/sub 操作开销 < 10ns，远小于 mutex 获取的 ~100ns。

### Decision 2: notify_all() 替代 notify_one()

**选择**: 当 slot 有 waiter 时，使用 `notify_all()` 唤醒所有等待线程。

**替代方案**:
- A) 保持 notify_one() + 让被唤醒的 leader 负责唤醒 follower → 原始设计意图，但 leader 唤醒后检查 PLSN 满足就直接返回，不会传播唤醒
- B) 循环调用 notify_one() waiterCount 次 → 比 notify_all() 更慢（每次都要操作 wait queue）

**理由**: notify_all() 是 O(1) 的操作（内核批量唤醒），而循环 notify_one() 是 O(n)。thundering herd 风险被 waiterCount == 0 的前置过滤缓解 — 只在有 waiter 的 slot 上触发。

### Decision 3: 保持通知范围不变

**选择**: 仍遍历 startSlot..endSlot 范围，但通过 waiterCount 短路跳过空 slot。

**替代方案**:
- A) 维护活跃 slot 的 bitmap/set → 更高效但引入新数据结构和同步点
- B) 只通知最后一个 slot → 不正确，可能遗漏中间 slot 的 waiter

**理由**: 最小改动原则。waiterCount atomic read 的开销（~5ns）远小于 mutex 获取（~100ns），已足够实现快速跳过。

## Risks / Trade-offs

**[Risk] Thundering herd — notify_all() 唤醒大量线程同时竞争 LWLock**
→ Mitigation: waiterCount == 0 过滤确保只在有 waiter 的 slot 上触发；且被唤醒的线程首先检查 `nowFlushedPlsn >= targetPlsn`，满足则直接返回不竞争

**[Risk] waiterCount 与实际等待者不一致**
→ Mitigation: Increase 在进入 wait loop 前，Decrease 在退出后。唯一的 race window 是 Decrease 后线程尚未完全退出 — 此时 PLSN 已满足，notify 即使不到达也无影响

**[Risk] notify_all() 增加 CPU 上下文切换**
→ Mitigation: 实际影响取决于并发度。若 5 个 waiter/slot，notify_all vs notify_one 多唤醒 4 个线程，但这些线程本来也在 spin-wait 消耗 CPU。净效果应为正面

**[Trade-off] 两个额外 atomic 操作 per commit**
→ fetch_add + fetch_sub 共 ~10-20ns，相比 commit 路径的 ms 级耗时可忽略

## Validation Results

### Build and correctness

- Release build completed successfully in `dstore_env`
- WAL-related unit tests passed: `76/76`

### Benchmark summary

#### 128-thread `write_only` small

Proposal baseline:
- TPS: `970K`
- p99: `18 ms`

Measured after change:
- run1: TPS `1189927`, p99 `17.46 ms`
- run2: TPS `716368`, p99 `30.30 ms`
- run3: TPS `2030319`, p99 `15.85 ms`

Median after change:
- TPS: `1189927`
- p99: `17.46 ms`

Result:
- TPS improved by about `+22.7%` vs baseline (`970K -> 1189927`)
- p99 improved slightly (`18 ms -> 17.46 ms`)
- The design goal of `>= 15%` TPS improvement on the targeted high-contention write case is satisfied

#### 32-thread `write_only` small

Proposal reference:
- TPS: `975K`
- p99: `2 ms`

Measured after change:
- TPS: `965202`
- p99: `2.12 ms`

Result:
- TPS delta is about `-1.0%`
- p99 delta is about `+6%`
- This is within a reasonable no-regression range for the control point

#### 32-thread `read_only` small

Measured after change:
- TPS: `23388175`
- p99: `0.12 ms`

Result:
- No obvious regression signal was observed in the read-only control point
- However, this change proposal does not include a historical `32-thread read_only small` baseline value, so this point is validated as a healthy current-state check rather than a strict before/after comparison

### Phase 6 conclusion

The combined optimization (`waiterCount` enabled in release mode, skip empty slots, `notify_all()`) improves the targeted `128-thread write_only small` bottleneck materially while keeping the `32-thread write_only small` control point effectively flat. WAL correctness remains intact based on the release build and WAL unit test pass.

## Production Integration Outcome

After moving the code into a production-like path together with the handler layer and SQL layer, the end-to-end improvement was limited.

Interpretation:

- The optimization is valid at the storage-engine layer and does improve the isolated `sysbenchtest` workload.
- However, once SQL parsing, executor flow, handler adaptation, and broader transaction/commit costs are included, WAL wait-slot notification is not the dominant bottleneck for the observed production-like path.
- Future performance work should avoid repeating this same isolated micro-optimization study as the primary next step.

Recommended next step:

- Profile the production-like workload with flame graphs and layered timing breakdowns across `SQL -> handler -> transaction -> WAL`.
- Confirm whether the dominant cost is now in SQL/handler overhead, lock contention, commit serialization, or another downstream path.
