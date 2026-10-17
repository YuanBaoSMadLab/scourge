# Scourge 博客示例

单文件博客服务器，无依赖无框架，编译产物拷走即用。

## 启动

- `start_vm.bat` — VM 解释执行，改代码即改即跑
- `start_exe.bat` — AOT 单文件 exe（不存在时自动编译）

两种模式输出逐字节一致。浏览器打开 http://127.0.0.1:18080

## 路由

- `/` 首页（文章卡片）
- `/post/<slug>` 文章页
- `/archive` 归档
- `/about` 关于
- `/static/style.css` 样式
- 其余 404

处理 64 个请求后自动退出（示例进程生命周期）。

## 结构

- `main.sg` 服务器入口 + 路由
- `layout.sg` HTML 渲染（转义 / mini markdown / 页面模板）
- `posts.sg` 文章数据（新文章在五个列表各加一项，正文支持 `## 标题`、`- 列表`、`> 引用`、空行分段）
- `static/style.css` 莫兰迪配色样式
