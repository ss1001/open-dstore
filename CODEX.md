# CODEX.md

This file is optimized for Codex-style work in this repository: fast orientation, safe execution, and minimal context switching.

## TL;DR

- Project: standalone C++14 storage engine under the `DSTORE` namespace.
- Primary entry point: [`include/framework/dstore_instance.h`](/Users/shesong/work/code/dstore/include/framework/dstore_instance.h), especially `StorageInstance`.
- Build order matters: build `utils/` first, then build `dstore`.
- Preferred environment: Docker container with the pinned toolchain and local libs mounted in.
- Before building manually, load [`buildenv`](/Users/shesong/work/code/dstore/buildenv).
- Test builds use `tmp_build/` and commonly recreate it from scratch.
- Git hooks are enabled by `buildenv`; commit message validation exists and may be sensitive to local tool differences.

## Working Rules

- Keep all new code inside the `DSTORE` namespace unless surrounding code clearly does otherwise.
- Follow existing include layering:
  - Public-facing headers live in `interface/`.
  - Internal shared headers live in `include/`.
  - Implementations live in `src/`.
- Match the existing style and naming of the touched module instead of introducing a new local convention.
- Prefer minimal, module-local changes over cross-cutting refactors unless the task requires broader edits.
- Validate changes with the smallest meaningful test target first.

## Fast Paths

### Enter the recommended environment

```bash
bash docker_deploy.sh --shell
```

Or attach to a running container:

```bash
docker exec -it dstore-dev bash
```

### Load environment

```bash
source buildenv
```

What this sets up from [`buildenv`](/Users/shesong/work/code/dstore/buildenv):

- `BUILD_ROOT` defaults to the repo root.
- `LOCAL_LIB_PATH` defaults to `../local_libs`.
- `CC` / `CXX` point to GCC 7.3 under `local_libs/buildtools/gcc7.3`.
- `core.hooksPath` is set to `.githooks`.

### Build `utils`

```bash
cd utils
bash build.sh -m release
```

Expected artifact:

```bash
utils/output/lib/libgsutils.so
```

### Build dstore

```bash
bash build.sh -m release
```

Useful flags from [`build.sh`](/Users/shesong/work/code/dstore/build.sh):

- `-m`: `debug|release|memcheck|coverage`
- `-st`: use system toolchain on/off
- `-co`: extra CMake options
- `-vb`: verbose build
- `-tm`: `ut|fuzz|perf|tpcc|lcov`

Expected artifacts:

```bash
output/lib/libdstore.so
output/lib/libdstore.a
```

## Test Shortcuts

### Unit tests

```bash
cd tests
bash build_and_run_ut.sh -t "$LOCAL_LIB_PATH" -u ../utils/output
```

Filter specific tests:

```bash
bash build_and_run_ut.sh -t "$LOCAL_LIB_PATH" -u ../utils/output -g 'UTBtree*.*,UTWal*.*'
```

Notes from [`tests/build_and_run_ut.sh`](/Users/shesong/work/code/dstore/tests/build_and_run_ut.sh):

- Rebuild defaults to `true`.
- It recreates `tmp_build/`.
- ASan mode is controlled by `-a`.
- The binary runs from `tmp_build/bin/unittest`.

### TPCC test

```bash
cd tests
bash build_and_run_tpcctest.sh -t "$LOCAL_LIB_PATH" -u ../utils/output
```

Notes from [`tests/build_and_run_tpcctest.sh`](/Users/shesong/work/code/dstore/tests/build_and_run_tpcctest.sh):

- Recreates `tmp_build/`.
- Builds with `-DDSTORE_TEST_TOOL=ON`.
- Mutates `guc.json` buffer size from `300000` to `655360`.
- Clears `tpccdir` before execution.

### Sysbench simulation test

```bash
cd tests
bash build_and_run_sysbenchtest.sh -t "$LOCAL_LIB_PATH" -u ../utils/output
```

Notes from [`tests/build_and_run_sysbenchtest.sh`](/Users/shesong/work/code/dstore/tests/build_and_run_sysbenchtest.sh):

- Intended to run inside the Docker environment.
- Recreates `tmp_build/`.
- Builds with `-DDSTORE_TEST_TOOL=ON`.
- Mutates `guc.json` buffer size from `300000` to `655360`.
- Clears `sysbenchdir` before execution.

## Repo Map

### Core layout

- [`interface/`](/Users/shesong/work/code/dstore/interface): public API and structs for external callers.
- [`include/`](/Users/shesong/work/code/dstore/include): internal headers shared across modules.
- [`src/`](/Users/shesong/work/code/dstore/src): implementation, generally mirroring `include/`.
- [`utils/`](/Users/shesong/work/code/dstore/utils): foundational utility library and a hard prerequisite for the main build.
- [`tests/`](/Users/shesong/work/code/dstore/tests): UT, TPCC, sysbench, and supporting test utilities.
- [`tools/`](/Users/shesong/work/code/dstore/tools): diagnostics such as page/WAL inspection.
- [`build_script/`](/Users/shesong/work/code/dstore/build_script): runtime/build config inputs.
- [`cmake/`](/Users/shesong/work/code/dstore/cmake): CMake modules and generated-config templates.

### High-value modules

- `buffer`: buffer pool, page writer, dirty page flow, checkpoints.
- `wal`: logging, durability, recovery-related paths.
- `transaction`: xact lifecycle, snapshots, ownership, state.
- `heap`: table storage mutation and scan paths.
- `index`: B-tree and related index machinery.
- `lock`: lock manager and lock primitives.
- `framework`: instance lifecycle, thread/session management, visible-thread registration.
- `control`: control files, tablespace and metadata persistence.
- `common`: algorithms, memory, logging, instrumentation, datatypes.

## Entry Points

Start with [`include/framework/dstore_instance.h`](/Users/shesong/work/code/dstore/include/framework/dstore_instance.h) when you need to understand lifecycle or subsystem wiring.

Most important methods to orient around:

- `Initialize`
- `Destroy`
- `Bootstrap`
- `StartupInstance`
- `StopInstance`
- `ShutdownInstance`
- `OpenPDB`
- `ClosePDB`
- `CreateThreadAndRegister`
- `UnregisterThread`

When tracing runtime behavior, `StorageInstance` is often the quickest route to related managers such as buffer, lock, thread, PDB, and stats components.

## Build And Runtime Facts Worth Remembering

- `LOCAL_LIB_PATH` must exist before `build.sh` runs or the build exits early.
- `tmp_build/` is disposable and often rebuilt from scratch by test scripts.
- Test scripts may modify generated build artifacts or runtime config inside the build directory.
- Some scripts assume Linux container tooling such as `/proc/cpuinfo` and GNU-like utilities.

## Git Notes

- Loading [`buildenv`](/Users/shesong/work/code/dstore/buildenv) also enables repository hooks via `.githooks`.
- The commit message hook enforces a structured format with `Description`, optional `TicketNo`, and required `Module`.
- If the local environment lacks compatible `grep` support for the hook implementation, hook behavior may fail even with a correct message.

## Editing Heuristics

- For narrow bug fixes, inspect the matching trio of paths first:
  - `include/<module>/...`
  - `src/<module>/...`
  - `tests/...`
- For interface changes, check whether the public contract belongs in `interface/` instead of only `include/`.
- For lifecycle or threading issues, begin from `framework/` before diving into leaf modules.
- For performance or benchmark work, expect related touchpoints in `tests/sysbenchtest`, `tests/tpcctest`, `include/common/instrument`, and module-local perf helpers.

## Suggested Workflow

1. Read the smallest relevant module surface and its nearest tests.
2. Confirm build/test path before editing.
3. Make the smallest coherent change.
4. Run the narrowest useful validation.
5. If broader impact is likely, expand to adjacent test coverage or build mode.

## If Information Conflicts

- Trust repository scripts and headers over secondary guidance docs.
- Treat this file as an execution-oriented map, not the source of truth.
- When in doubt, inspect the actual script or header being exercised.
