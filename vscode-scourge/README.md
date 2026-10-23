# Scourge Language — VS Code 扩展

Scourge 语言的 VS Code 支持：语法高亮、一键运行、AOT 构建、实时纠错（编译器级诊断 + 快速修复）与运行级调试。

## 功能

| 功能 | 说明 | 触发方式 |
|---|---|---|
| 语法高亮 | 关键字 / 类型 / 内建函数 / 装饰器 / docstring 契约（requires/ensures/invariant）/ 转义序列 | 打开 `.sg` / `.sgt` 自动启用 |
| 实时纠错 | `sg check --json` 全量验证：错误码、行内区间、关联 notes、fixits 快速修复；保存即查 + 输入防抖 600ms 后台查 | 自动 |
| 快速修复 | 编译器给出的替换区间直接转 CodeAction（灯泡提示） | Ctrl+. |
| 运行 | 集成终端执行 `sg run <file>`（可交互，支持 `input()`） | Ctrl+Alt+R / 命令面板 "Scourge: Run File" |
| AOT 构建 | `sg build <file>`，产物落 `<源目录>/out/` | Ctrl+Alt+B / 命令面板 |
| REPL | 打开会话式求值终端（`:help` 查看会话命令） | 命令面板 "Scourge: Open REPL" |
| 手动检查 | 对当前文件立即跑 check 并汇总结果 | 命令面板 "Scourge: Check File" |
| 调试 | DAP：启动 / 终止 / 重启 / 调试控制台输出流 / panic 行可点击跳转源码 | F5 |

## 调试

`.vscode/launch.json`：

```json
{
  "version": "0.2.0",
  "configurations": [
    {
      "type": "scourge",
      "request": "launch",
      "name": "Scourge: Run File",
      "program": "${workspaceFolder}/main.sg",
      "sgPath": "d:/Scourge/build-rel/scg.exe"
    }
  ]
}
```

- `program` 必填；`args` 可传附加参数（如 `["--no-cache"]`）；`sgPath` 缺省用 `scg` on PATH（编译器交付名，32f3374 起由 sg.exe 改名以避让 miniconda 同名工具）。
- **panic 跳转**：进程崩溃时，调试控制台的 panic 行（`panic: ... at <函数> (<文件>:<行>)`）带源码定位，点击即跳。
- **断点边界（诚实声明）**：Scourge 运行时（VM 与 AOT）当前没有 PC 级停机/单步钩子，因此本扩展不提供断点与单步——断点图标会显示为未绑定（灰色）。这是语言运行时的能力边界，不是扩展缺陷；待运行时提供调试钩子后可在 `sgdebug.js` 中平移接入。

## 设置

| 设置 | 默认 | 说明 |
|---|---|---|
| `scourge.executablePath` | `""` | scg 可执行文件路径；留空按 工作区 `build-rel/scg.exe` → `build/scourge.exe` → PATH 探测 |
| `scourge.checkOnSave` | `true` | 保存时运行 check |
| `scourge.checkOnType` | `true` | 编辑防抖 600ms 后台 check |
| `scourge.checkTimeoutMs` | `10000` | 单次 check 超时（毫秒） |

## 安装（开发机）

```powershell
npx --yes @vscode/vsce package --no-dependencies
code --install-extension scourge-lang-0.1.0.vsix
```

## 诊断数据流

```
编辑器事件 ──防抖──▶ scg check --json <file>
                        │ stdout: [{code,severity,message,file,line,col,
                        │           end_line,end_col,fixits[],notes[]}]
                        ▼
             sgcheck.js ──▶ VS Code Diagnostics（1 基 → 0 基映射）
                        ├──▶ notes → RelatedInformation
                        └──▶ fixits → CodeAction 快速修复
```

JSON 字段是编译器 `DiagEngine` 的导出契约（`src/Common/Diagnostic.cpp` 的 JSON writer），扩展按契约消费、不猜测。
