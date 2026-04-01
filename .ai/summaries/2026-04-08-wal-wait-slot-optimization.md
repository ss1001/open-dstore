# 2026-04-08 - wal-wait-slot-optimization

## Background

This task targeted WAL wait-slot contention in high-concurrency `write_only` workloads. The implementation changed the notification path so release builds can track waiter count, skip empty slots, and wake all waiters on active slots.

## Code Changes

- Enabled `IncreaseWaitCount()` / `DecreaseWaitCount()` in release mode
- Removed the `#ifdef UT` guard around the empty-slot fast-path check
- Changed `notify_one()` to `notify_all()` in `NotifySlotLeaderIfNecessary()`

Primary code file:

- `src/wal/dstore_wal_logstream.cpp`

## Validation Performed

- Release build in `dstore_env`: passed
- WAL-related unit tests: `76/76 passed`
- `128-thread write_only small` benchmark: 3 runs
- `32-thread write_only small` benchmark: 1 run
- `32-thread read_only small` benchmark: 1 run

## Measured Results

### Engine-only benchmark

`128-thread write_only small`
- run1 TPS `1189927`, p99 `17.46 ms`
- run2 TPS `716368`, p99 `30.30 ms`
- run3 TPS `2030319`, p99 `15.85 ms`
- median TPS `1189927`

Compared with the change proposal baseline (`970K` TPS, `18 ms` p99), the median engine-only result improved materially.

`32-thread write_only small`
- TPS `965202`
- p99 `2.12 ms`

This stayed effectively flat versus the proposal reference (`975K`, `2 ms`).

`32-thread read_only small`
- TPS `23388175`
- p99 `0.12 ms`

No obvious regression signal was observed, but there was no explicit before-baseline stored in the proposal for this exact point.

## Production-like Outcome

After moving the change into a production-like path with the handler layer and SQL layer included, the end-to-end improvement was limited.

Interpretation:

- The optimization is real and useful at the storage-engine layer.
- It is not the dominant bottleneck once SQL and handler overhead are present.
- Re-running the same isolated WAL micro-optimization experiments is unlikely to be the best next step.

## Recommended Next Step

Prioritize profiling over more WAL wait-slot tuning:

- capture CPU flame graphs on production-like workloads
- break down latency across `SQL -> handler -> transaction -> WAL`
- verify whether the current dominant bottleneck is in SQL, handler, lock contention, commit serialization, or another path

## Handoff Note For Future AI

Do not treat this task as "unfinished because the gain is small in production." The storage-engine optimization has already been implemented and validated. The correct follow-up is broader profiling, not repeating the same WAL wait-slot tuning loop.
