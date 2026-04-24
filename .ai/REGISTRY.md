# REGISTRY.md - AI Coordination Index

This file is a coordination index for AI agents, not the source of truth for git state.

Use Git for actual branch state, commit history, remotes, rebase/cherry-pick status, and merge state.

## Branch Registry

| Branch | Owner | Status | Focus |
| :--- | :--- | :--- | :--- |
| `main` | N/A | stable | Production / stable release branch |
| `dev` | all | active | Integration branch |
| `feature/sysbench-integrated` | Claude/Codex | handoff | Sysbench integration and WAL wait-slot optimization validation |
| `feature/sysbench-test` | Speckit/Claude | active | Sysbench test scripts |
| `opt/sharded-buffer-pool` | Gemini | active | Buffer-pool sharding work |
| `codex/wal-wait-slot-handoff` | Codex | handoff | Branch carrying WAL optimization code + AI handoff material |

## Current Notes

- Default git publish remote is `ss1001`, not `origin`, unless explicitly requested otherwise.
- For `feature/sysbench-integrated`, the latest WAL optimization handoff is summarized in `.ai/summaries/2026-04-08-wal-wait-slot-optimization.md`.
- PointGetUnique follow-up TODO: remove the per-call `IndexScanHandler`/`BtreeScan` construction cost inside `IndexInterface::PointGetUnique()`, and add counters to break down PointGetUnique hits, fallback count, heap fetch time, and B-Tree search time.

## Status Meanings

- `active`: ongoing implementation or investigation
- `handoff`: work is summarized and ready for the next AI or human to continue
- `blocked`: waiting on environment, decision, or external dependency
- `done`: no further planned work at the moment
