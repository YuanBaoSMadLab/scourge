# sget — Scourge 包管理器

用 Scourge 自举写的生态工具。`sget.json`（registry 索引）驱动主流库安装。

## 用法

```
sget search [词]          浏览 registry（包名/API 类型/描述）
sget install <pkg>        按 registry 条目安装（sqlite3 opencv torch qt ...）
sget install <url>        安装 .sg 绑定文件（URL 直链）
sget list / uninstall     管理已装包
```

安装流程：curl 下载 → tar 解压 → DLL 按 registry 的 dlls[] 部署到
`packages/<pkg>/bin` 与项目根 → 绑定文件 → `sget.lock.json`。

## API 类型与绑定策略（诚实边界）

| api | 含义 | 绑定 |
|---|---|---|
| c | 纯 C API | 直接可用（@extern 声明即可） |
| c-legacy | C 兼容层符号 | 子集可用 |
| cpp/com | C++/COM ABI | **须 C shim**（extern "C" 包装 DLL），安装只放库文件 |

- **sqlite3** [c]：绑定直接可用，`sqlite_demo.sg` 是真装真调的闭环演示
- **opencv** [c-legacy]：官方预编译包保留 C API 子集，内置绑定
- **onnxruntime / yolo / torch / qt / webview2**：库文件可装；调用需 C shim
  工程（C++/COM 的名字修饰与对象模型无法被 FFI 直调——cgo/dart:ffi 同款约束）

## FFI 契约

- str 参数 = 只读指针（const char*）；原地改写缓冲区的 API 不可绑定
- str 返回 = 深拷贝（NULL → ""）
- 输出指针参数（sqlite3_open 的 db**）待指针内建后开放
- int 句柄往返（HWND/sqlite3* 一类不透明指针按 int64 传）可直接用
