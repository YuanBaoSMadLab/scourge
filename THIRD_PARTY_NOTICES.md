# 第三方组件声明（THIRD_PARTY NOTICES）

本项目分发包（源码仓库、预编译库包、便携可执行包）包含或链接以下第三方组件，
各组件版权归其原作者所有，遵循其原始许可证分发。

## LLVM / MLIR

- 项目：<https://llvm.org>（含 MLIR 子项目）
- 版权：Copyright (c) The LLVM Authors 及各贡献者
- 许可证：Apache License 2.0 with LLVM Exceptions
  （全文见库包内 `LICENSE.TXT`，或上游
  <https://github.com/llvm/llvm-project/blob/main/LICENSE.TXT>）
- 使用方式：
  1. **预编译库包**（Release 附件 `scourge-mlir-libs-*`）：由上游未修改源码
     构建的 LLVM+MLIR 静态库、头文件与 CMake 配置，随包附带上游
     `LICENSE.TXT` 与第三方声明。
  2. **源码构建**：`scripts/bootstrap.sh` / `bootstrap.ps1` 拉取上述库包；
     用户亦可自备本机 LLVM/MLIR（`--llvm-root`）。
  3. **AOT 产物运行时**：`scourge build` 生成的目标文件经 clang/lld 链接，
     链接产物亦受上述许可证约束的接口头文件影响（LLVM Exception 明确
     允许以此方式分发二进制而无需开源使用方自己的代码）。
- 本项目未修改 LLVM/MLIR 源码；若未来引入修改，将按 Apache-2.0 第 4(b)
  条在对应文件头保留显著修改声明。

## SQLite（examples/bindgen/packages/sqlite3 示范包）

- 项目：<https://sqlite.org>（amalgamation 单文件发行版 `sqlite3.c` / `sqlite3.h`）
- 版权：SQLite 处于公有领域（Public Domain），详见
  <https://sqlite.org/copyright.html> 与源码头注释
- 使用方式：`examples/bindgen/packages/sqlite3/`（本地工作树目录，按仓库
  `.gitignore` 约定不入 git）保存上述两个未修改的 vendored 源文件，用于
  bindgen 生态示范与 `verify/verify_bindgen.ps1` 回归（生成绑定 → 代理
  DLL → 双模 demo）。该包为示例而非 scourge 运行时的必需组件；若未来经
  分发包（zip 等）对外分发，需随包保留 SQLite 的公有领域声明。
- 公有领域组件无许可证分发义务；保留本声明仅为溯源清晰。

## 许可证要点（面向分发者与二次开发者）

- 允许商用、闭源集成、再分发；唯一义务是随分发保留上述许可证文本与版权声明。
- LLVM Exception 保证：链接 LLVM 库的产物（含 scourge 及其 AOT 编译输出）
  不因此被要求开源。
- 本项目自身的许可证请见仓库根目录 `LICENSE`（如尚未添加，发布前由维护者确定，
  建议 MIT 或 Apache-2.0）。
