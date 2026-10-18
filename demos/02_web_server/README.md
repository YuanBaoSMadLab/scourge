# 02 · 完整博客站（网络方向）

单二进制前后端完整的博客服务：服务端渲染详情页 + JSON API + 内嵌 CSS/前端
（首页 fetch 动态渲染列表）+ 异步 PV 统计。后台线程经 channel 聚合全部路由
的 PV，SYNC 消息应答栅栏保证读视图确定性，QUIT 哨兵 + `thread_join` 收尾。

## 路由

| 方法 | 路径 | 行为 |
|---|---|---|
| GET | `/` | 骨架页（前端 fetch `/api/posts` 动态渲染文章列表） |
| GET | `/style.css` | 内嵌样式表 |
| GET | `/post/{id}` | SSR 详情页（标题/正文/阅读数服务端直出） |
| GET | `/api/posts` | JSON 文章列表（含各自阅读数） |
| POST | `/api/posts` | JSON 发文入库（201；坏 JSON/缺字段 400） |
| GET | `/api/stats` | JSON 统计：served/paths/hits/Top3 |
| 其余 | — | 404 JSON |

程序固定处理 **8 个请求**后自动退出（便于自动化验证）。

## 构建与运行

```powershell
powershell -File build.ps1          # 构建 + 启动（另开终端 curl 打请求）
# 手动：
scg build demos\02_web_server\main.sg
demos\02_web_server\out\main.exe
```

## 模块结构（多文件 import 协作）

| 文件 | 职责 | 关键能力 |
|---|---|---|
| `main.sg` | 装配入口：持有全部可变状态（posts/views/双 channel）+ accept 循环 + 线程编排 | `capability net, thread`、`thread_spawn`/`channel` 栅栏与哨兵、`json_decode` |
| `views_html.sg` | 纯 HTML/CSS 模板（骨架页/详情页/样式表/404） | 三引号 f-string、内嵌 `<script>` fetch |
| `json_api.sg` | 纯 JSON 响应体构造 | f-string `{{ }}` 转义、跨模块只读传参 |
| `stats_hub.sg` | 纯统计排名（TopN 定宽键降序） | 定宽键技巧复用 |

## 实测输出（VM/AOT 双模逐字节一致，8 请求哈希全同）

```
blog-up port=18099
blog-down requests=8 paths=6 hits=8
top /api/posts = 3
top /style.css = 1
top /post/99 = 1
```

```
GET  /              → 200  首页 HTML（fetch('/api/posts') 动态渲染文章列表）
GET  /style.css     → 200  内嵌样式表
GET  /post/1        → 200  SSR 详情页：<h1>hello scourge</h1>…<p>1 views - <a href="/">back</a></p>
GET  /post/99       → 404  <h1>404</h1>
GET  /api/posts     → 200  [{"id":1,"title":"hello scourge","views":1}, …]
POST /api/posts     → 201  {"id":4,"title":"fourth post"}          # JSON 发文入库
GET  /api/posts     → 200  列表含第 4 篇（POST 即时可见）
GET  /api/stats     → 200  {"served":8,"paths":6,"hits":8,"top":[["/api/posts",3],…]}
```

## 语言要点

- **可变状态集中在 main 模块**：List/Dict 跨模块只读传参安全，
  修改操作（posts.append、views 计数）全部留在持有方——避免跨模块
  共享全局变量。
- **SYNC 栅栏模式**：业务消息单向流入 worker；读共享 Dict 前发
  `"SYNC"` 并等 `"ok"` 应答对齐，保证双模输出确定；`"QUIT"` 哨兵 +
  `thread_join` 收尾。
- 网络服务为**单线程 accept 循环**（官方契约），异步统计不阻塞响应。
