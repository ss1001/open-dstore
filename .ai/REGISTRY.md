# REGISTRY.md - AI Task Management

This registry tracks which AI agent is working on which branch and their current progress.

## Branch Registry

| Branch Name | Agent (Estimated) | Status | Task Description |
| :--- | :--- | :--- | :--- |
| `main` | N/A | Stable | Production / Stable release branch. |
| `dev` | ALL | Active | Integration branch for all features. |
| `feature/sysbench-integrated`| Claude/Codex | Active | Sysbench integration plus WAL wait-slot optimization validation; engine-only gain confirmed, but production-like end-to-end impact was limited after handler and SQL layers were included. Prefer profiling over repeating the same micro-optimization loop. |
| `feature/sysbench-test` | Speckit/Claude | Active | Writing and debugging Sysbench test scripts. |
| `opt/sharded-buffer-pool` | Gemini | Active | Optimizing buffer pool via sharding. |

## Active Sync Conflicts
- **None currently identified.**

## Synchronization History
- **2026-03-31**: Initial registry created by Gemini CLI.
- **2026-03-31**: Removed `jemalloc` and `001-dstore-arch-analysis` branches.
- **2026-04-08**: WAL wait-slot optimization validated. Isolated storage-engine benchmark improved, but production-like path with handler + SQL showed limited end-to-end benefit. Future AI work should prioritize profiling and layered bottleneck analysis.
