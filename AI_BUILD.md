# AI_BUILD.md

Read this file only when the task requires compilation, testing, benchmarking, or runtime validation.

## Rules

- Run build, test, and benchmark work inside `dstore_env`.
- Do not compile on the host.
- Build or run the smallest scope that matches the task.
- Skip compilation for doc-only, handoff-only, git-only, or analysis-only work unless explicitly requested.

## Container Setup

Enter the container:

```bash
docker exec -it dstore_env bash
```

Load environment:

```bash
source /etc/bash.bashrc
source buildenv
```

`buildenv` sets:

- `BUILD_ROOT`
- `LOCAL_LIB_PATH`
- `CC` / `CXX` for GCC 7.3
- `.githooks` via `core.hooksPath`

If `cmake` fails with `GLIBCXX_3.4.26 not found`, trim the injected GCC runtime from `LD_LIBRARY_PATH` before building or testing:

```bash
export LD_LIBRARY_PATH=$(printf "%s" "$LD_LIBRARY_PATH" | tr ":" "\n" | grep -v "/gcc/lib64" | paste -sd ":" -)
```

## Main Build

Build `utils` first:

```bash
cd utils
bash build.sh -m release
```

Then build dstore:

```bash
cd ..
bash build.sh -m release
```

Expected artifacts:

```bash
utils/output/lib/libgsutils.so
output/lib/libdstore.so
output/lib/libdstore.a
```

Useful `build.sh` flags:

- `-m`: `debug|release|memcheck|coverage`
- `-st`: system toolchain on/off
- `-co`: extra CMake options
- `-vb`: verbose build
- `-tm`: `ut|fuzz|perf|tpcc|lcov`

## Test Shortcuts

### Unit tests

```bash
cd tests
bash build_and_run_ut.sh -t "$LOCAL_LIB_PATH" -u ../utils/output
```

Filter example:

```bash
bash build_and_run_ut.sh -t "$LOCAL_LIB_PATH" -u ../utils/output -g 'UTBtree*.*,UTWal*.*'
```

Script behavior:

- Rebuild defaults to `true`
- Recreates `tmp_build/`
- Runs `tmp_build/bin/unittest`

### Sysbench

```bash
cd tests
bash build_and_run_sysbenchtest.sh -t "$LOCAL_LIB_PATH" -u ../utils/output
```

Script behavior:

- Recreates `tmp_build/`
- Uses `-DDSTORE_TEST_TOOL=ON`
- Rewrites `guc.json` buffer from `300000` to `655360`
- Clears `sysbenchdir`

### TPCC

```bash
cd tests
bash build_and_run_tpcctest.sh -t "$LOCAL_LIB_PATH" -u ../utils/output
```

Script behavior:

- Recreates `tmp_build/`
- Uses `-DDSTORE_TEST_TOOL=ON`
- Rewrites `guc.json` buffer from `300000` to `655360`
- Clears `tpccdir`

## Reminders

- `LOCAL_LIB_PATH` must exist before `build.sh` runs.
- `utils/` must be built before dstore.
- `tmp_build/` is disposable.
- Some scripts assume Linux utilities such as `/proc/cpuinfo`.
- Prefer a narrow UT filter or a single benchmark point over a full matrix unless the task needs broad validation.
