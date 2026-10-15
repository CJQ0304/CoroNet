# CoroNet

**一个从零实现的 C++ 网络库：在「主从 Reactor + one loop per thread」之上自研 C++20 无栈协程调度，
并组装成一个可压测的 HTTP/1.1 服务器。**

C++20 · epoll · eventfd · timerfd · 无第三方依赖（仅 libstdc++ + pthread） · 实测环境 WSL2 + GCC 15.2

---

## 一、这个项目要解决什么

C++ 网络编程里最常见的组合是 **「epoll + 线程池 + Reactor」** —— 它本身没有错，但写法高度趋同：
业务逻辑被拆散进回调与状态机，"一个请求的完整流程"在源码上不再相邻。

CoroNet 的差异点在**协程**：同一套 Reactor 地基上，把异步 IO 从"回调 / 状态机"改写成
**`co_await` 的顺序代码**。于是"一个连接一个协程"里，**解析状态、缓冲区、响应对象全都成了协程的局部变量** ——
不需要成员变量，也不需要手写续延。

要强调的是：**协程不是替代多线程，而是叠加在多线程之上。** 两者解决的是不同问题 ——
多线程负责"把多个核用起来"，协程负责"让每个核上的并发连接写得下去"。这个项目两条都做了，
并且都用实测数据量化了收益（见第五节）。

## 二、四个技术亮点

| # | 亮点 | 关键实现 | 难点与取舍 |
| - | --- | --- | --- |
| 1 | **C++20 无栈协程层** | `Task<T>`（惰性、可 `co_await`）/ `DetachedTask`（立即启动、结束自毁）+ Awaiter；`co_await conn->asyncRead()` / `asyncWriteSegments()` | 手写 `promise_type` / `await_ready·suspend·resume`，并要算清协程帧的分配与生命周期 |
| 2 | **协程超时与取消** | `CoroOpState`（`shared_ptr` 共享，`done/cancelled/timedOut` **一次性裁决**）+ `.withTimeout(10s)` | 三个唤醒源（epoll 回调 / 定时器超时 / 连接关闭）竞争同一状态，必须做到不重入、不泄漏 |
| 3 | **异步日志双缓冲** | 前端只 `memcpy` 进 `Buffer`，后端**锁外** `fwrite`；靠**所有权转移**而非加锁保护数据 | 难点在于"不持锁的前提下如何保证数据不被写坏"，以及缓冲满时的策略取舍 |
| 4 | **增量 HTTP 解析 + `writev` 聚集写** | 半包/流水线状态机（含 414/431/413 防滥用）；响应**头体分离**，`writev` 每响应一次系统调用 | "为什么不能收全了再解析"；头体分离的真正目的是给 `writev` 铺路；用 A/B 对照把收益量化到系统调用层：计数口径限定为"连接套接字上的写"，以剥离每响应一次的 eventfd 唤醒；分片写会踩 Nagle + delayed-ACK，已在 accept 后设 `TCP_NODELAY` |

## 三、架构

```text
main thread ─────────────────────────────────────────────────────────────
  main()
   └─ EventLoop(baseLoop)
        ├─ Acceptor(listenFd)         accept() 新连接
        └─ EventLoopThreadPool        round-robin 派发连接
                    │
                    │ eventfd 唤醒目标线程（跨线程任务队列）
                    ▼
IO 线程 × N ──────────────────────────────────────────────────────────────
  EventLoop × N              one loop per thread
   └─ epoll_wait   ← 水平触发（LT）
        ├─ Channel → TcpConnection(fd)    连接落户后只属于这个线程
        │    └─ 协程：co_await conn->asyncRead() / asyncWriteSegments()
        └─ timerfd                        sleepFor() / withTimeout() 的定时器
                    │
                    ▼
   挂起后由三者之一唤醒：epoll 事件 / 定时器超时 / 连接关闭（取消）
```

数据流（以一次 HTTP 请求为例）：

```text
epoll_wait ─► TcpConnection 回调 ─► 唤醒协程 ─► co_await asyncRead()  读一包
     ─► HttpContext 增量解析（半包则继续读） ─► handleRequest()（纯同步函数）
     ─► co_await asyncWriteSegments(头, 体)  ← writev 一次发出去
     ─► keep-alive 则回到循环，否则 shutdown()
```

## 四、目录结构

| 路径 | 内容 |
| --- | --- |
| `server/` | **库本体 15 个编译单元 + 3 个可执行程序 + `tests/`（GoogleTest，37 例）** |
| `bench-scaling.sh` | 扩展性曲线（0/1/2/4/8 个 IO 线程 × 3 次取中位，逐轮轮转以抵消功耗墙漂移） |
| `bench-ab.sh` | `writev` 的 A/B 对照（编出"逐段 `write`"的退化版，用 `strace -yy` 数每响应写系统调用） |

`server/` 的关键文件：

| 文件 | 作用 |
| --- | --- |
| `channel` / `poller` / `event_loop` | 事件单元 / epoll 封装 / 事件循环（poll → 分发 → 执行待办） |
| `event_loop_thread(_pool)` | one loop per thread；`setThreadNum(0)` 退化为纯单线程 |
| `acceptor` / `tcp_connection` / `tcp_server` | 监听 / 连接全生命周期（最复杂，含协程 IO）/ 组装 |
| `task.hpp` / `sleep.hpp` / `coro_error.hpp` | 协程核心（`Task`、`sleepFor`、超时取消错误类型） |
| `buffer` | 应用层缓冲（倍增扩容 + 自动收缩 + `readv`/`writev` 直读直写） |
| `logger` / `async_logging` / `log_file` | 异步日志（双缓冲）+ 文件写入 |
| `http_request` / `http_parser` / `http_response` | 请求对象 / 增量状态机 / 响应（头体分离） |
| `echo_server` · `coro_echo_server` · `http_server` | 三个可执行程序（回调式 echo / 协程式 echo / HTTP 服务） |

## 五、性能（本机实测，可复现）

同一份代码的**两个工作点** —— 不是"谁更好"，而是"换了什么代价换到了什么"：

| 工作点 | 服务端 IO 线程 | 客户端 | QPS（3 次中位） | P50 | P99 |
| --- | --- | --- | --- | --- | --- |
| **A · 低并发** | 4 | `wrk -t4 -c100 -d10s` | **1,019,953** | **80 µs** | **197 µs** |
| **C · 高并发** | 8 | `wrk -t8 -c500 -d30s` | **1,252,430** | **309 µs** | **1.00 ms** |

**多线程的收益也是量化的**（客户端固定 `-t4 -c100 -d10s`，只改服务端 IO 线程数）：

| IO 线程 | 1 | 2 | **4** | 8 |
| --- | --- | --- | --- | --- |
| QPS（中位） | 271,838 | 466,547 | **965,730** | 795,746 |
| 相对 1 loop | 1.00× | 1.72× | **3.55×** | 2.93× |
| P99 | 654 µs | 448 µs | **219 µs** | 232 µs |

> **1 → 4 个 loop 换来 3.55× 吞吐、P99 从 654 µs 降到 219 µs。** 这才是"多线程"三个字的
> 实际含金量 —— 关键不在"用了多线程"，而在于**量化了它值多少、以及从哪开始不值**。
> 拐点在 4 之后：8 个 loop 在 `-c100` 下反而低于 4 loop（每 loop 只有 12.5 条连接，喂不饱），
> 但在 `-t8 -c500` 下 8 loop = 1,252,430，**赢过** 4 loop 的 796,813 —— 所以准确说法是
> **"8 个 loop 需要更多在飞请求才喂得饱"**，而不是"8 线程更慢"。
> 表中数值按**逐轮轮转配置顺序**测得（避免固定排在最后的配置被功耗墙系统性低估）。

> ⚠️ **两张表里唯一重叠的一格请对照着读**：「4 个 IO 线程 + `-t4 -c100 -d10s`」出现了两次
> —— **1,019,953**（上表 A）与 **965,730**（下表 4），P99 也分别是 197 µs / 219 µs。
> 它们是**不同批次**的测量（同一参数在不同批次之间 QPS 极差可达 17%）。
> 所以**同一张表内横向比才有意义，跨批次只比数量级**。

测试环境：i9-14900HX（32 逻辑核，混合架构）/ WSL2 + Ubuntu 26.04.1 / 内核 6.18.54.1-microsoft-standard-WSL2 /
g++ 15.2.0 `-O2` Release / wrk 4.1.0 / 客户端与服务端**同机（回环）**。

> ⚠️ **两点需要说明**：
> ① 回环压测反映的是**协议栈 + 调度上限**，不等于真实网络吞吐；一个字节要在收发两侧各算一遍。
> ② 单次 10s 波动可达 10%~30%（三组数据里第 1 次最高：A 高 12%、C 高 29%，且**三次单调递减**
> = 笔记本 PL2→PL1 功耗墙），所以表中一律取 **3 次中位数**，且只在同一轮内横向比较。

> **另一组对照回答了"为什么加压反而更慢"**：把客户端从 `-t4 -c100` 提到 `-t8 -c500`，
> 吞吐反而掉 22%（1,019,953 → 796,813）、P50 差 7 倍（80 µs → 572 µs）。
> 用 Little's 定律算在飞请求：`1.02M × 80 µs ≈ 82` vs `797K × 572 µs ≈ 456` ——
> **并发涨了 5.5 倍而吞吐下降**，说明不是"客户端造不出压力"，而是服务端能有效服务的请求变少了。
> 归因按贡献排序：**① 服务端并行度没跟上**（请求的并发需求涨 5 倍，服务端仍是 4 个 loop）
> 与 **② 每 loop 连接数 25 → 125**（工作集超出 L2，每请求 cache miss 上升）。
> 证据是直接改服务端：**同一客户端参数下把 IO 线程 4 → 8，吞吐立刻 +57%、P50 降到 309 µs**。
> 判据补充：`nproc` = 32，三组分别只忙 **8 / 12 / 16** 个线程，**CPU 从未接近饱和** ——
> 所以不能拿"CPU 被客户端抢走"来解释（谈资源不足之前先量利用率）。

> **另有一组不靠 QPS 立论的优化对照**：把响应头/体两段用一次 `writev` 写出，改成逐段 `write`
> （只改这一处，其余一行不动），再用 `strace -yy` 只数**连接套接字上**的写系统调用 ——
> **每响应 2 次 → 1 次**（实测 **1.001 vs 2.002** 次/响应，确定性计数、重复跑不变）。
> 分片写还会触发 **Nagle 与 delayed-ACK 的 40 ms 惩罚**
> （退化版 QPS 101 万 → 2,264，而进程 CPU 占用仅 5.4% —— 它在等，不在算），
> 据此为每条连接启用 `TCP_NODELAY`。复现命令见第六节。

## 六、快速开始

> 依赖：Linux（需要 `epoll`/`eventfd`/`timerfd`，WSL2 可用）、GCC ≥ 11 或 Clang ≥ 14（C++20 协程）、CMake ≥ 3.16。

```bash
cd server
cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j

# HTTP/1.1 服务（默认 8080，第二个参数是 IO 线程数）
./build/http_server 8080 4
curl -v http://127.0.0.1:8080/plain                 # → 200 OK，body "OK"

# 另两个可执行程序：回调式 echo / 协程式 echo（默认 8888）
./build/echo_server 8888 4
./build/coro_echo_server 8888 4
nc 127.0.0.1 8888
```

HTTP 路由（故意只留能验证功能的最小集合）：`GET /`（首页 HTML）、`GET /plain`（压测入口）、
`GET /hello?msg=x`、`GET /echo?msg=x`、`POST /echo`（回显 body）、其它 → 404、非 GET/POST → 405。

压测（**在 Linux 本地文件系统下跑**，不要把服务端目录放在 `/mnt/*` 这类挂载盘上，
跨盘 IO 慢且数据失真；两个脚本都会检测并警告）：

```bash
sudo apt install -y wrk
wrk -t4 -c100 -d10s --latency http://127.0.0.1:8080/plain     # --latency 不能省，否则没有 P50/P99

# 扩展性曲线（0/1/2/4/8 个 IO 线程 × 3 次取中位）
# 脚本默认操作 $HOME/projects/CoroNet/server，克隆到别处时用 SRV_DIR 指到自己的路径
cd .. && SRV_DIR="$(pwd)/server" bash bench-scaling.sh
```

A/B 对照与单元测试（都用可选开关，默认不参与构建）：

```bash
cd server

# 量化 writev 的收益：编出"只把 writev 退化成逐段 write"的对照版，再用 strace 数系统调用
# （指标是**连接套接字上**的写调用次数：`strace -yy` 按 fd 过滤，剥离每请求一次的 eventfd 唤醒）
cmake -B build -DCMAKE_BUILD_TYPE=Release -DCORONET_BUILD_AB_DEGRADE=ON
cmake --build build -j
cd .. && SRV_DIR="$(pwd)/server" bash bench-ab.sh

# 单元测试（GoogleTest）：http_parser 22 例 + buffer 15 例 = 37 例
sudo apt install -y libgtest-dev
cd server
cmake -B build -DCMAKE_BUILD_TYPE=Release -DCORONET_BUILD_TESTS=ON
cmake --build build -j && (cd build && ctest --output-on-failure)
```

## 七、关键设计取舍（TL;DR）

- **epoll 用水平触发（LT），评估后明确不切 ET。** LT 与协程"一次 `co_await` 读一次"语义对齐；
  ET 的"循环读到 `EAGAIN`"必须跨越协程挂起点。瓶颈在 syscall 次数与堆分配，
  已用 `readv`/`writev` + 对象复用解决，而不是靠改触发模式。
- **日志走双缓冲 + 所有权转移，而不是加锁保护缓冲。** 前端（业务线程）只做 `memcpy`，
  后端（写盘线程）锁外 `fwrite`；缓冲的所有权在两者间转移，所以写盘时没有第二个线程能碰它。
- **`writev` 的前提是"头体分离"。** 响应对象刻意拆成 header/body 两段，
  就是为了让"状态行 + 头部 + body"一次系统调用发出去。
- **`handleRequest()` 是纯同步函数。** 协程的价值只体现在"等数据"上；数据齐了以后的处理逻辑
  就该是普通函数 —— 这样才可单测。

## 八、参考与致谢

设计思路参考了 muduo（one loop per thread、`Channel`/`Poller` 分层）、sylar（日志双缓冲）、
cppcoro 与 sogou/workflow（协程封装形态）。**代码为本人实现，未复制任何开源仓库的源码。**

---

**构建与测试状态**：WSL2 + GCC 15.2 下全部编译运行通过；单元测试 37/37 通过（GoogleTest）；
`writev` 的 A/B 对照为确定性计数结果（每响应 2 次 → 1 次写系统调用）。
