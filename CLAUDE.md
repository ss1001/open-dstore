# CLAUDE.md

## Core Facts

- Project: 独立的 C++14 数据库存储引擎，`DSTORE` 命名空间。
- 入口：`StorageInstance`（`include/framework/dstore_instance.h`）
- 代码布局：
  - `interface/`：公共 API
  - `include/`：内部共享头文件
  - `src/`：实现代码
  - `tests/`：UT / TPCC / Sysbench
  - `utils/`：前置工具库（需先编译）
  - `tools/`：诊断工具（pagedump, waldump, buflookup 等）
- 核心模块：`wal`, `buffer`, `transaction`, `index`, `heap`, `lock`, `framework`, `common`

## Default Behavior

- 先读相关模块最小表面，再动手。
- 改动保持最小和局部，匹配现有代码风格。
- 接口变更需检查是否属于 `interface/` 而非仅 `include/`。
- 文档、交接、Git、分析类任务不要编译或测试。

## Validation Rule

- 仅在任务确实需要运行时验证时才编译/测试。
- 需要编译时，统一阅读 `AI_BUILD.md`，在 Docker 容器 `dstore_env` 中执行，禁止宿主机编译。

## Git Notes

- 默认推送 remote：`ss1001`（非 `origin`），除非明确指定。
- `buildenv` 设置了 `.githooks`。
- Commit message 格式：`Description` / `TicketNo` / `Module`。

## AI Coordination

- 读 `.ai/REGISTRY.md` 了解分支/任务协调。
- 读 `.ai/SHARED_RULES.md` 了解跨 AI 规则。
- 完成结果放 `.ai/summaries/`。
