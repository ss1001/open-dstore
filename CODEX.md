# CODEX.md

Minimal Codex working context for this repository.

## Core Facts

- Project: standalone C++14 storage engine under the `DSTORE` namespace.
- Main entry point: `include/framework/dstore_instance.h` and `StorageInstance`.
- Main code layout:
  - `interface/`: public API
  - `include/`: internal shared headers
  - `src/`: implementation
  - `tests/`: UT / TPCC / sysbench
  - `utils/`: prerequisite utility library
- High-value modules: `framework`, `wal`, `transaction`, `buffer`, `heap`, `index`, `lock`, `common`.

## Default Behavior

- Read the smallest relevant module surface first.
- Keep changes minimal and local unless broader edits are required.
- Match existing style and conventions of the touched module.
- For interface changes, check whether the contract belongs in `interface/` rather than only `include/`.
- For doc-only, handoff-only, git-only, or analysis-only tasks, do not compile or test unless explicitly requested.

## Validation Rule

- Only build or test when the task actually requires runtime validation.
- When build/test/benchmark work is needed, read `AI_BUILD.md` and use `dstore_env`, not the host.

## Git Notes

- Default publish remote: `ss1001`, not `origin`, unless explicitly requested otherwise.
- Repository hooks are enabled via `buildenv`.
- Commit message hook expects structured `Description` / `TicketNo` / `Module`.

## AI Coordination

- Read `.ai/REGISTRY.md` for branch/task coordination.
- Read `.ai/SHARED_RULES.md` for stable cross-AI rules.
- Put substantial handoff results in `.ai/summaries/`.

## If Information Conflicts

- Trust repository scripts, headers, and actual code over secondary docs.
- Treat this file as a lightweight index, not the source of truth.
