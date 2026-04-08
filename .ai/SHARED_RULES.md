# SHARED_RULES.md - Multi-AI Development Standards

This file contains the mandatory rules that ALL AI agents (Gemini, Claude, Cursor, Speckit, etc.) must follow when working on the `dstore` project.

## 1. Environment Isolation
- **If build/test/benchmark/validation is needed**: run it inside `dstore_env`.
- **Do not compile on the host machine.**
- **If the task is doc-only, handoff-only, git-only, or analysis-only**: do not compile or test unless explicitly requested.

## 2. Coding Standards
- **Namespace**: All code must reside within the `DSTORE` namespace.
- **Headers**:
    - Public API: `#include "interface/..._interface.h"`
    - Internal: `#include "include/...h"`
- **Memory Management**: Prioritize `utils` memory contexts for allocation. Avoid raw `new`/`delete` or standard `malloc`/`free` unless necessary for third-party integration.
- **C++ Standard**: Strictly C++14.

## 3. Collaboration Protocol
- **Registry Check**: Before starting any task, an AI agent MUST read `.ai/REGISTRY.md` to ensure no branch or file conflicts exist.
- **Branch Ownership**: Respect branch naming conventions (e.g., `ai/<name>/<feature>`). Do not push to branches owned by other agents without explicit instruction.
- **Git Publish Default**: Unless the user explicitly says otherwise, push branches to remote `ss1001` and do not push to `origin`.
- **Summaries**: Upon finishing a significant task, create a summary file in `.ai/summaries/<task_name>.md` detailing changes and verification results.

## 4. Verification
- Run the smallest validation that matches the task.
- For code changes affecting correctness, prefer `tests/build_and_run_ut.sh`.
- For performance-related changes, run `tests/build_and_run_sysbenchtest.sh` when validation is required.
- For doc-only, handoff-only, git-only, or repository-maintenance changes, do not compile or test unless explicitly requested.
