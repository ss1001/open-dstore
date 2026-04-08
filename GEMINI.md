# GEMINI.md - Dstore 项目上下文指南

本文档为在 Dstore 代码库中进行开发和维护提供基础上下文和指令。Dstore 是一个使用 C++14 实现的独立、模块化数据库存储引擎。

## 项目概述

Dstore 旨在作为一个高性能、独立的存储引擎组件。它采用模块化架构，各个子系统（缓冲区管理、WAL、事务等）通过定义的接口进行隔离与集成。

- **主要技术栈:** C++14, CMake
- **核心命名空间:** `DSTORE`
- **主要入口点:** `StorageInstance` 类 (`include/framework/dstore_instance.h`)
- **核心依赖:** GCC 7.3, Huawei Secure C, LZ4, cJSON, Google Test.

## 架构与目录结构

项目在公共接口和内部实现之间保持严格的分离：

- `interface/`: 公共 API 头文件。包含供外部调用者使用的 `*_interface.h` 和 `*_struct.h`。
- `include/`: 模块间共享的内部头文件。
- `src/`: 实现文件，目录结构与 `include/` 对应。
- `utils/`: 基础工具库（前提依赖），提供内存上下文、并发原语和 VFS 抽象。
- `tests/`: 单元测试 (GTest) 和集成测试 (TPCC)。
- `tools/`: 诊断工具，如 `pagedump` 和 `waldump`。

### 核心模块
| 模块 | 职责 |
| :--- | :--- |
| **buffer** | 缓冲区池管理、页面置换 (LRU)、检查点 (Checkpoint)。 |
| **wal** | 预写日志，确保持久性和故障恢复。 |
| **transaction** | 事务生命周期管理、并发控制、快照。 |
| **heap** | 堆表管理（插入、删除、更新）。 |
| **index** | 索引结构实现（如 B-Tree）。 |
| **common** | 通用算法、内存管理和日志记录。 |
| **framework** | 实例、会话和线程管理。 |

## 编译与运行

- 只有在任务确实需要编译、测试、benchmark 或运行验证时，才执行构建动作。
- 如需编译或测试，统一查阅 `AI_BUILD.md`。
- 所有构建和验证都在 `dstore_env` 中执行，不在宿主机编译。

## 开发规范

- **命名空间:** 所有代码必须位于 `DSTORE` 命名空间内。
- **头文件包含:**
  - 公共 API 使用 `#include "interface/..._interface.h"`。
  - 内部实现细节使用 `#include "include/...h"`。
- **内存管理:** 优先使用 `utils` 库提供的内存上下文进行可控的分配和清理。
- **错误处理:** 使用 `include/common/error/` 和 `include/common/log/` 中定义的项目特定错误码和日志宏。
- **代码风格:** 遵循代码库中现有的模式（命名约定、缩进和文档风格）。
- **验证:** 只在任务需要时执行最小范围验证；需要细节时查看 `AI_BUILD.md`。新功能应在 `tests/unittest/` 目录下包含相应测试。
- **验证策略:** 文档类、交接类、Git 类、纯分析任务默认不编译、不测试，除非用户明确要求。

## Multi-AI 协作协议 (MACP)

本项目同时使用多个 AI 工具（Gemini, Claude, Speckit 等）。为了防止冲突，必须遵循以下规则：

1. **先读后写**: 在开始任何任务前，必须阅读 `.ai/REGISTRY.md` 和 `.ai/SHARED_RULES.md`。
2. **状态更新**: 开始新任务或切换分支时，必须更新 `.ai/REGISTRY.md` 中的状态。
3. **完成总结**: 任务完成后，在 `.ai/summaries/` 目录下创建一个总结文件，说明变更内容和验证结果。
4. **环境强制**: 需要编译或验证时，统一阅读 `AI_BUILD.md`，并在 Docker 容器 `dstore_env` 中执行。
