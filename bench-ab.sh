#!/usr/bin/env bash
# ============================================================================
#  CoroNet A/B 对照压测：一次 writev（优化版） vs 逐段 write（退化版）
#
#  对照目标
#    HTTP 响应是"头 + 体"两段内存。用**一次 writev** 把两段发出去，
#    相比"分两次 write"，省下的是「每响应少一次写系统调用」。
#    （没有对照实验，"优化了 writev"就只是一句无法验证的话。）
#
#  为什么主指标不是 QPS
#    本项目单次 10s 压测的抖动就有 10%~30%（实测同一配置三次 QPS 差 29%），
#    "每响应少一次系统调用"这种效应量会被噪声完全盖住。所以：
#      · 主指标 = 每响应**连接套接字上的写系统调用次数** —— strace -yy 精确计数，零噪声
#        ⚠️ 必须按 fd 过滤（用 strace -yy 才能取到 TCP: 注解），不能把进程里所有 write 都算上：进程里还有别的 write ——
#           · **每响应一次的 eventfd 唤醒**：协程总是在 doPendingFunctors() 里被恢复
#             （finishWriteOp 用 queueInLoop 排 resume，tcp_connection.cpp:278），
#             所以协程函数体的整个执行窗口内 callingPendingFunctors_ == true；
#             而响应又恰好在这同一窗口内同步写完（writev 快路径，tcp_connection.cpp:240），
#             于是写完成路径里那次 queueInLoop 命中 event_loop.cpp:132 的唤醒分支
#             -> ::write(eventfd, &one, 8) 就真落地了。
#           · 异步日志落盘（fwrite -> write）
#        不过滤的话两版会各自多出约 1 次/响应，把结论从"1 次 vs 2 次"
#        污染成"2.011 vs 3.006 次"（不按 fd 过滤时实测到的数）。
#        结论要引的是"发响应"那部分，所以只数 TCP 套接字上的写；
#        其他 fd 的写会照样打印出来，但标注为"不计入"，保持证据透明。
#      · 辅指标 = 每响应的**服务端 CPU 时间** —— 但要注意：服务端被压满时，
#        CPUΔ 两版都 ≈ 墙钟 × IO 线程数，于是"每响应 CPU" ≈ 常数 ÷ QPS，
#        它和 QPS 是**同一信息的两种表达，不是独立证据**；
#        只有服务端 CPU 尚有余量时，它才携带 QPS 之外的信息。
#      · QPS 只作参考列，并明确标注"受噪声影响，不用于结论"
#
#  两个必须满足的前提（不满足的话量到的是别的东西，不是系统调用开销）
#    ① 连接必须设 TCP_NODELAY。分片写（先头、再体）会踩 Nagle + delayed-ACK：
#       第 2 段 < MSS 且前段未被 ACK ⟹ 被 Nagle 攒住；对端只收到半个响应、
#       解析不出完整内容 ⟹ 触发约 40ms 的 delayed ACK。每请求凭空多出 ~40ms。
#       本项目 acceptor.cpp 的 setTcpNoDelay() 已处理。实测不设它时，
#       退化版 QPS 从 100 万掉到 2264（慢 448 倍）、进程 CPU 占用只有 5%（全在等，不在算）。
#    ② 性能轮与计数轮都要带 CORONET_LOG_QUIET=1，两轮的差异只能是"挂不挂 strace"。
#
#  为什么分两轮跑（这一步不做，数据就是错的）
#    性能轮：**不带** strace 跑 —— 测 QPS 与 CPU 时间。
#       strace 会给每次系统调用插两次 ptrace 停等，把被测进程的 CPU 时间
#       从"程序开销"污染成"跟踪开销"，此时测 CPU 是自欺欺人。
#    计数轮：**带** strace 只数次数 —— 计数是确定性的，跑一次就够，慢无所谓。
#
#  用法（在 Linux / WSL 里跑；服务端目录别放在 /mnt/* 这类挂载盘上）
#      # 先编出两个版本
#      cd server
#      cmake -B build -DCMAKE_BUILD_TYPE=Release -DCORONET_BUILD_AB_DEGRADE=ON
#      cmake --build build -j
#      # 再跑对照（SRV_DIR 默认 $HOME/projects/CoroNet/server，克隆到别处请显式指定）
#      cd .. && SRV_DIR="$(pwd)/server" bash bench-ab.sh
#
#  可调环境变量
#      PORT=8080  THREADS=4  CLIENT_T=4  CLIENT_C=100  DURATION=5s  RUNS=3
#      BUILD_DIR=build   （想和单元测试的构建目录隔离时，改成 build-ab 之类）
# ============================================================================
set -u

SRV_DIR="${SRV_DIR:-$HOME/projects/CoroNet/server}"
BUILD_DIR="${BUILD_DIR:-build}"
PORT="${PORT:-8080}"
THREADS="${THREADS:-4}"
CLIENT_T="${CLIENT_T:-4}"
CLIENT_C="${CLIENT_C:-100}"
DURATION="${DURATION:-5s}"
RUNS="${RUNS:-3}"
URL="http://127.0.0.1:${PORT}/plain"
OUT="${OUT:-ab-$(date +%Y%m%d-%H%M%S).log}"

# 注意：如果和单元测试共用同一个 build/ 目录，CMake 缓存里的
# `-DCORONET_BUILD_TESTS=ON` 会一直保留（要能配得通就得装着 GoogleTest）。
# 想彻底隔离就用 BUILD_DIR=build-ab 另开一个构建目录。
BIN_OPT="$SRV_DIR/$BUILD_DIR/http_server"
BIN_DEG="$SRV_DIR/$BUILD_DIR/http_server_nowritev"

say()  { printf '%s\n' "$*"; }
die()  { printf '错误：%s\n' "$*" >&2; exit 1; }
rule() { printf '%s\n' "--------------------------------------------------------------------"; }

# 读进程累计 CPU 时间（utime+stime，单位 jiffies）。读不到就什么都不输出，
# 由调用方按 "n/a" 处理 —— /proc 在非 Linux 环境下不存在，不能因此中断整个脚本。
cpu_ticks() {
    local pid="$1"
    [ -r "/proc/$pid/stat" ] || return 0
    awk '{ print $14 + $15 }' "/proc/$pid/stat" 2>/dev/null
}

median() {
    awk '{v[NR]=$1} END{
        if (NR == 0) { print "NA"; exit }
        n = NR
        for (i = 1; i <= n; i++)
            for (j = i + 1; j <= n; j++)
                if (v[j] < v[i]) { t = v[i]; v[i] = v[j]; v[j] = t }
        if (n % 2) print v[(n + 1) / 2]
        else       printf "%.1f", (v[n/2] + v[n/2+1]) / 2
    }'
}

wait_listen() {
    local port="$1" limit="$2" i=0
    local tries=$(( limit * 10 ))
    while [ "$i" -lt "$tries" ]; do
        if [ -x "$(command -v ss 2>/dev/null)" ]; then
            ss -ltn 2>/dev/null | grep -q ":${port}[[:space:]]" && return 0
        else
            (exec 3<>"/dev/tcp/127.0.0.1/${port}") 2>/dev/null && { exec 3>&-; return 0; }
        fi
        i=$(( i + 1 ))
        sleep 0.1
    done
    return 1
}

SRV_PID=""
STRACE_PID=""
TMP=""

# 杀掉当前正在跑的服务端。
#
# ⚠️ 两个必须避开的坑（都属于"看着正常、实际自伤"的类型）：
#  ① 不能用 `pkill -x http_server_nowritev`：Linux 的进程名（comm）上限是 15 个字符，
#     "http_server_nowritev" 会被截断成 "http_server_now"，-x 精确匹配永远匹配不上。
#  ② 更不能用裸的 `pkill -f "$BIN"`：**strace 的命令行里就包含被判程序的路径**
#     （strace -f -e ... -o out /path/build/http_server 8080 4），这一刀会连 strace
#     一起砍掉 -> 脚本自身被打断，strace 的输出也可能不完整。
#  所以优先按**父子关系**精确杀：计数轮里 http_server 是 strace 的子进程，
#  性能轮里它就是 $SRV_PID 本身。最后才用 pgrep 兜底清残留，并显式排除自己。
kill_server() {
    local p did=0
    if [ -n "${SRV_PID:-}" ]; then
        kill "$SRV_PID" 2>/dev/null
        SRV_PID=""
        did=1
    fi
    if [ -n "${STRACE_PID:-}" ]; then
        local kids
        kids="$(pgrep -P "$STRACE_PID" 2>/dev/null)"
        if [ -n "$kids" ]; then
            # shellcheck disable=SC2086
            kill $kids 2>/dev/null      # 杀被测程序，strace 会随其退出而正常收尾并落盘
        else
            kill "$STRACE_PID" 2>/dev/null
        fi
        STRACE_PID=""
        did=1
    fi
    # 兜底只用 pgrep，且**只在没有已知 PID 时**执行（也就是每轮开始前的清场）。
    # 如果每次都跑兜底，它会连 strace 一起匹配到（strace 命令行含被判程序路径），
    # 把正在收尾的 strace 强杀，导致 -o 文件里的系统调用记录不完整。
    if [ "$did" -eq 0 ]; then
        for p in $(pgrep -f "${BIN_OPT:-/nonexistent}" 2>/dev/null) \
                 $(pgrep -f "${BIN_DEG:-/nonexistent}" 2>/dev/null); do
            case "$p" in
                "$$"|"$PPID") continue ;;   # 绝不要杀掉脚本自己或它的父 shell
            esac
            kill "$p" 2>/dev/null
        done
    fi
    sleep 0.3
    return 0
}

cleanup() {
    kill_server
    # 2>/dev/null：某些受限环境里 rm 被包装过，会把"删临时目录"的提示打到 stderr，
    # 看着像脚本出错。临时目录删不掉不影响结论，静音即可。
    [ -n "${TMP:-}" ] && rm -rf "$TMP" 2>/dev/null
    return 0
}
trap cleanup EXIT INT TERM

# ---------------------------------------------------------------------------
#  0. 前置检查
# ---------------------------------------------------------------------------
[ -d "$SRV_DIR" ] || die "找不到服务端目录：$SRV_DIR（可用 SRV_DIR=... 指定）"
[ -x "$BIN_OPT" ] || die "找不到优化版：$BIN_OPT
     先编译：cd $SRV_DIR && cmake -B $BUILD_DIR -DCMAKE_BUILD_TYPE=Release && cmake --build $BUILD_DIR -j"
[ -x "$BIN_DEG" ] || die "找不到退化版：$BIN_DEG
     它需要显式打开开关：
     cd $SRV_DIR && cmake -B $BUILD_DIR -DCMAKE_BUILD_TYPE=Release -DCORONET_BUILD_AB_DEGRADE=ON
     cmake --build $BUILD_DIR -j"
command -v wrk    >/dev/null 2>&1 || die "没有 wrk：sudo apt install -y wrk"
command -v strace >/dev/null 2>&1 || die "没有 strace：sudo apt install -y strace"
# 预检 -yy：套接字的 `<TCP:[...]>` 注解只有 -yy（两个 y）才给。
# 只带 -y 时套接字会写成 `<socket:[N]>` —— 计数照样能走通（分类器认它），
# 但少了解析信息；真要是老 strace 连 -yy 都不认，就地失败，别等跑完性能轮才发现。
strace -yy -o /dev/null true >/dev/null 2>&1 \
    || say "⚠️  本机 strace 似乎不支持 -yy，将退回只识 <socket:[N]> 的口径（计数仍然有效）"

case "$SRV_DIR" in
    /mnt/*) say "⚠️  警告：服务端在 $SRV_DIR（/mnt 下），跨盘 IO 会让压测数据失真。"
            say "    建议换到 \$HOME/projects/CoroNet/server 下跑。" ;;
esac

CLK_TCK="$(getconf CLK_TCK 2>/dev/null || echo 100)"
TMP="$(mktemp -d)"
cd "$SRV_DIR" || die "无法进入 $SRV_DIR"

say "================================================================"
say " CoroNet A/B 对照：一次 writev  vs  逐段 write"
say "================================================================"
say " 优化版   : $BIN_OPT"
say " 退化版   : $BIN_DEG"
say " 服务端   : $THREADS 个 IO 线程   （port $PORT）"
say " 客户端   : wrk -t$CLIENT_T -c$CLIENT_C -d$DURATION（两版完全一致）"
say " 轮次     : 性能轮 $RUNS 次 + 计数轮 1 次，两个版本各来一遍"
say " 计数口径 : 只数**连接套接字**上的写（strace -yy 按 fd 类型过滤）"
say " 原始输出 : $SRV_DIR/$OUT"
say "================================================================"
say

{
    echo "### CoroNet A/B 对照 $(date '+%F %T')"
    echo "cpu=$(lscpu 2>/dev/null | awk -F': +' '/Model name/{print $2; exit}')  nproc=$(nproc 2>/dev/null)"
    echo "kernel=$(uname -r)  gxx=$(g++ --version 2>/dev/null | head -1)"
    echo "server_threads=$THREADS  client=-t$CLIENT_T -c$CLIENT_C -d$DURATION  runs=$RUNS"
    echo "note=性能轮不带 strace；计数轮带 strace -yy（strace 会改变 CPU 时间，所以分两轮）"
    echo "note=写调用只统计连接套接字（<TCP:...>）上的，唤醒 eventfd 与日志文件的写不计入"
} >> "$OUT"

# ---------------------------------------------------------------------------
#  1. 性能轮（不带 strace）：QPS + 每响应 CPU 时间
# ---------------------------------------------------------------------------
perf_run() {
    local bin="$1" tag="$2" r res qps reqs t0 t1
    : > "$TMP/qps.$tag"
    : > "$TMP/cpu.$tag"

    for r in $(seq 1 "$RUNS"); do
        kill_server
        CORONET_LOG_QUIET=1 "$bin" "$PORT" "$THREADS" >> "$OUT" 2>&1 &
        SRV_PID=$!

        if ! wait_listen "$PORT" 15; then
            say "  [$tag] rep=$r/$RUNS  ✗ 服务端 15 秒内没监听 $PORT，跳过"
            kill_server; SRV_PID=""
            continue
        fi
        sleep 1   # 等各 IO 线程把 epoll / eventfd / timerfd 都建好再压

        t0="$(cpu_ticks "$SRV_PID")"
        res="$(wrk -t"$CLIENT_T" -c"$CLIENT_C" -d"$DURATION" --latency "$URL" 2>&1)"
        t1="$(cpu_ticks "$SRV_PID")"

        printf '\n===== perf %s rep=%s =====\n%s\n' "$tag" "$r" "$res" >> "$OUT"

        qps="$(printf '%s\n' "$res" | awk '/Requests\/sec/{print $2; exit}')"
        reqs="$(printf '%s\n' "$res" | awk '/requests in/{print $1; exit}')"

        kill_server; SRV_PID=""

        if [ -z "$qps" ]; then
            say "  [$tag] rep=$r/$RUNS  ✗ wrk 没给出 Requests/sec（看 $OUT）"
            continue
        fi
        printf '%s\n' "$qps" >> "$TMP/qps.$tag"

        # jiffies -> 每请求微秒： Δtick / CLK_TCK / 请求数 * 1e6
        # 注意 printf 必须带 \n：不带的话每次追加都会和上一次的输出粘成一行
        # （沙箱里实测踩到：两次数值变成 "0.0000.000"）。
        # d == 0 说明这个环境根本读不到进程 CPU 时间（例如非 Linux 的 /proc），
        # 宁可留空让汇总显示 n/a，也不要写一个假的 0.000。
        if [ -n "$t0" ] && [ -n "$t1" ] && [ -n "${reqs:-}" ]; then
            awk -v d="$(( t1 - t0 ))" -v r="$reqs" -v hz="$CLK_TCK" \
                'BEGIN{ if (r + 0 > 0 && d + 0 > 0) printf "%.3f\n", d / hz / r * 1000000 }' \
                >> "$TMP/cpu.$tag"
        fi

        say "  [$tag] rep=$r/$RUNS  QPS=$qps  requests=${reqs:-?}  cpuΔ=$(( ${t1:-0} - ${t0:-0} )) tick"
    done
}

# ---------------------------------------------------------------------------
#  2. 计数轮（带 strace）：数 write / writev 系统调用次数
# ---------------------------------------------------------------------------
# 解析失败时把现场信息一次性打全 —— 否则还得自己再跑一遍 head 才能定位，
# 而"解析失败"恰恰是最容易让人反复排查的 bug 类型。
diag_strace() {
    local f="$1" tag="$2" n b c
    n="$(wc -l < "$f" 2>/dev/null)"; n=${n:-0}
    n="$(printf '%s' "$n" | tr -d '[:space:]')"
    b="$(grep -c 'writev[(]' "$f" 2>/dev/null)"; b=${b:-0}
    c="$(grep -c 'write[(]'  "$f" 2>/dev/null)"; c=${c:-0}
    say "       ── 现场诊断（[$tag]）────────────────────────────"
    say "       文件          : $f"
    say "       总行数        : $n"
    say "       strace 版本   : $(strace --version 2>/dev/null | head -1)"
    say "       含 'writev('  : $b        含 'write(' : $c"
    if [ "$n" -gt 0 ] 2>/dev/null; then
        # 关键：要打**含 writev( 的头几行**，而不是文件头几行。
        # 文件前几行永远是启动期的"写日志/写 eventfd"，看不出套接字长什么样；
        # 真正要看的是发响应那几行上的 fd 注解（`<TCP:...>` 还是 `<socket:[N]>` 还是啥都没有）。
        say "       前 3 行含 'writev(' 的行（截到 110 列）:"
        awk '/writev[(]/{ printf "         | %s\n", substr($0, 1, 110); if (++k == 3) exit }' "$f" 2>/dev/null
        say "       前 3 行含 'write('（排除 writev）的行:"
        awk '/write[(]/ && !/writev[(]/{ printf "         | %s\n", substr($0, 1, 110); if (++k == 3) exit }' "$f" 2>/dev/null
        say "       行首前 12 列的模式分布（最多 5 种）:"
        cut -c1-12 "$f" 2>/dev/null | sort | uniq -c | sort -rn | head -5 \
            | awk '{ printf "         | %6s 行  「%s」\n", $1, $2 }'
    fi
    say "       ────────────────────────────────────────────────"
}

count_run() {
    # 注意：不能写成 `local bin="$1" tag="$2" sfile="...$tag..."` ——
    # 同一句 local 里的 $tag 此时还没赋值，set -u 下会直接报 unbound variable。
    local bin="$1"
    local tag="$2"
    # 放在 $SRV_DIR 而不是 $TMP：$TMP 会被 cleanup 整个 rm -rf，
    # 而解析出问题时恰恰最需要这份原始输出。`*.log` 已在 .gitignore 里。
    local sfile="$SRV_DIR/strace.$tag.log"
    local spid res reqs per per_raw anfile f1 f2 f3 sock_total
    local sock_wv sock_w evt_w file_w oth_w nn_w nn_wv raw_wv raw_w

    kill_server
    : > "$sfile"

    # 用 strace **直接启动**服务端：这样 strace 是它的父进程，
    # 不需要放宽 ptrace_scope，也用不着 sudo。
    # CORONET_LOG_QUIET=1 必须与性能轮一致：两轮之间唯一的差异应该只是"挂不挂 strace"。
    # -yy（注意是**两个 y**）：少一个 y 就分不出"哪些 write 在发响应"。
    #   `-y`  只做 readlink(/proc/pid/fd/N)，对套接字只能拿到 `socket:[12345]`；
    #         路径类 fd 会得到 `<路径>`、eventfd 会得到 `<anon_inode:[eventfd]>` ——
    #         唯独**套接字拿不到 `TCP:`**，于是"哪些 write 是在发响应"根本分不出来。
    #   `-yy` 才会额外用 netlink 解析出 `<TCP:[127.0.0.1:8080->127.0.0.1:39112]>`。
    #   实测：只带 -y 时，发响应的 writev 全被归进"其他 fd"，脚本正确报了自检失败。
    CORONET_LOG_QUIET=1 strace -f -yy -e trace=write,writev -o "$sfile" "$bin" "$PORT" "$THREADS" >> "$OUT" 2>&1 &
    spid=$!
    STRACE_PID=$spid   # 让 kill_server 按父子关系精确杀掉被测程序

    if ! wait_listen "$PORT" 20; then
        say "  [$tag] ✗ strace 下的服务端没起来（看 $OUT）"
        kill_server; wait "$spid" 2>/dev/null
        return 1
    fi
    sleep 1

    res="$(wrk -t"$CLIENT_T" -c"$CLIENT_C" -d"$DURATION" --latency "$URL" 2>&1)"
    reqs="$(printf '%s\n' "$res" | awk '/requests in/{print $1; exit}')"
    printf '\n===== count %s =====\n%s\n' "$tag" "$res" >> "$OUT"

    kill_server            # 杀掉 http_server -> strace 随之退出并落盘
    wait "$spid" 2>/dev/null

    # ── 计数：只数**连接套接字**上的写（理由见文件头"为什么主指标不是 QPS"） ──
    # ① 行首格式随输出目标而变，三种都要能数到，所以**不锚行首**：
    #      terminal  [pid 12345] writev(...)   输出到终端（stderr）
    #      file      12345 writev(...)         输出到文件（-o）：裸 PID + 一个空格，无方括号
    #      none      writev(...)               主线程那行常常连 PID 都不带
    #    用「非 [A-Za-z0-9_] 字符（或行首）+ write( / writev(」：
    #      · `writev(` 不会误配 `write(`（write 后面是 'v' 不是 '('）；
    #      · `pwrite(` / `process_vm_writev(` 因紧邻字符是字母而被排除。
    # ② 不必剔除 `<unfinished ...>`。一次调用被拆成两行打印时长这样 ——
    #      writev(9<TCP:...>, [...] <unfinished ...>   <- 含 "writev("
    #      <... writev resumed>) = 41                  <- 不含 "writev("
    #    只数含 "writev(" 的行，天然既不漏也不重。
    #    （若按"是否含 unfinished"过滤，反而会把唯一可计数的那一行删掉。）
    # ③ 按 fd 注解分类（这就是 `-yy` 的作用）：
    #      <TCP:[...]>              = 发响应  ← 只有这部分计入"每响应写调用"
    #      <socket:[N]>             = 套接字，但 strace 没能解析出协议（只带了 -y 就是这么写）
    #                                 —— 也当套接字算，见下面注释
    #      <anon_inode:[eventfd]>   = 跨线程唤醒（每响应都会触发一次）
    #      <路径>                    = 异步日志落盘
    anfile="$TMP/an.$tag"
    awk '
    {
        if (match($0, /(^|[^A-Za-z0-9_])writev\(/)) raw_wv++
        if (match($0, /(^|[^A-Za-z0-9_])write\(/))  raw_w++
    }
    {
        if (match($0, /(^|[^A-Za-z0-9_])(writev|write)\([0-9]+<[^,]*>/)) {
            tok = substr($0, RSTART, RLENGTH)
            sub(/^[^A-Za-z0-9_]/, "", tok)
            sc = (tok ~ /^writev\(/) ? "writev" : "write"
            anno = tok
            sub(/^[A-Za-z]+\(/, "", anno)      # -> 9<TCP:[...]>
            sub(/^[0-9]+</, "", anno)
            sub(/>$/, "", anno)
            # 套接字判定必须同时认这两种：
            #   · `TCP:[...]`  —— 带了 -yy，strace 解析出了协议（正常情况）
            #   · `socket:[N]` —— 只带了 -y，readlink 只能给出 socket inode。
            #     这是个**兜底**，专门防止"忘了 -yy"时把发响应误归到"其他 fd"。
            #     （只带 -y 时 sock 恒为 0，脚本自检会拦下并拒绝出数。)
            if      (anno ~ /^(TCP|UDP|UNIX|socket):/) cls = "sock"
            else if (anno ~ /eventfd/) cls = "evt"
            else if (anno ~ /timerfd/) cls = "tmfd"
            else if (anno ~ /\//)      cls = "file"
            else                       cls = "oth"
            cnt[cls " " sc]++
            if (!(cls in samp)) samp[cls] = anno
        } else if (match($0, /(^|[^A-Za-z0-9_])(writev|write)\([0-9]+/)) {
            # 没带 fd 注解（strace 未带 -yy 或该 fd 已关闭）—— 单独归类，不计入结论
            tok = substr($0, RSTART, RLENGTH)
            sub(/^[^A-Za-z0-9_]/, "", tok)
            sc = (tok ~ /^writev\(/) ? "writev" : "write"
            cnt["noname " sc]++
        }
    }
    END {
        for (k in cnt)  print k, cnt[k]
        for (k in samp) print "samp " k " " samp[k]
        print "raw writev", raw_wv + 0
        print "raw write",  raw_w  + 0
    }' "$sfile" > "$anfile" 2>/dev/null

    sock_wv=0; sock_w=0; evt_w=0; file_w=0; oth_w=0
    nn_w=0; nn_wv=0; raw_wv=0; raw_w=0
    while read -r f1 f2 f3; do
        case "$f1 $f2" in
            "sock writev")   sock_wv="${f3:-0}" ;;
            "sock write")    sock_w="${f3:-0}" ;;
            "evt write")     evt_w="${f3:-0}" ;;
            "evt writev")    evt_w=$(( evt_w + ${f3:-0} )) ;;
            "tmfd write")    oth_w=$(( oth_w + ${f3:-0} )) ;;
            "tmfd writev")   oth_w=$(( oth_w + ${f3:-0} )) ;;
            "file write")    file_w="${f3:-0}" ;;
            "file writev")   file_w=$(( file_w + ${f3:-0} )) ;;
            "oth write")     oth_w=$(( oth_w + ${f3:-0} )) ;;
            "oth writev")    oth_w=$(( oth_w + ${f3:-0} )) ;;
            "noname write")  nn_w="${f3:-0}" ;;
            "noname writev") nn_wv="${f3:-0}" ;;
            "raw writev")    raw_wv="${f3:-0}" ;;
            "raw write")     raw_w="${f3:-0}" ;;
            *) : ;;
        esac
    done < "$anfile"

    sock_total=$(( sock_wv + sock_w ))
    per="$(awk -v r="${reqs:-0}" -v s="$sock_total" \
        'BEGIN{ if (r+0>0) printf "%.3f", s/r; else print "n/a" }')"
    per_raw="$(awk -v r="${reqs:-0}" -v a="$raw_wv" -v b="$raw_w" \
        'BEGIN{ if (r+0>0) printf "%.3f", (a+b)/r; else print "n/a" }')"

    # 自检：请求确实跑完了，却一个"连接套接字上的写"都数不到 —— 那是**解析失败**。
    # 静默打一个 0.000 会把整个对照结论带偏，所以这里不但要喊出来，还要
    # ① 打印现场诊断（省掉一轮往返）；② **拒绝写结果文件** ——
    #    汇总读不到 cnt.$tag 就显示 n/a，而不是一个看着很确定的假数字。
    if [ "$sock_total" -eq 0 ] && [ "${reqs:-0}" -gt 0 ]; then
        say "  ⚠️  [$tag] 完成 ${reqs} 个请求，却一个「连接套接字上的写」都没数到。"
        say "       常见原因：strace 只带了 -y 没带 -yy（套接字只注解成 <socket:[N]>，"
        say "       解析不出 TCP:），或输出格式与预期不符。"
        say "       原始输出已保留：$sfile"
        # 把分类结果原样打出来 —— 一眼就能看出那 8 万次写被归到了哪个桶。
        # 缺 -yy 时，套接字的写会全部落进「oth」桶，这种形态一眼可见。
        say "       ── 分类明细（本轮的 fd 注解怎么被归类的）────────"
        while read -r d1 d2 d3; do
            say "       | $d1 $d2 $d3"
        done < "$anfile"
        say "       ─────────────────────────────────────────────"
        diag_strace "$sfile" "$tag"
        return 1
    fi
    if [ "$(( nn_w + nn_wv ))" -gt 0 ]; then
        say "  ⚠️  [$tag] 另有 $(( nn_w + nn_wv )) 行 write/writev 没有 fd 注解，未计入结论。"
    fi

    printf '%s %s %s %s %s %s %s %s\n' \
        "${reqs:-0}" "$sock_wv" "$sock_w" "$evt_w" "$file_w" "$oth_w" "$raw_wv" "$raw_w" \
        > "$TMP/cnt.$tag"
    say "  [$tag] requests=${reqs:-0}   套接字写：writev=$sock_wv + write=$sock_w  ->  $per 次/响应"
    say "         其他 fd（不计入）：eventfd 唤醒 $evt_w 次 / 日志文件 $file_w 次 / 其他 $oth_w 次"
    say "         未过滤 fd 的原始计数：writev $raw_wv + write $raw_w = $per_raw 次/响应"
    say "         文件：$sfile"
    return 0
}

say "[1/2] 性能轮（不带 strace）：QPS + 每响应 CPU 时间"
perf_run "$BIN_OPT" opt
say
perf_run "$BIN_DEG" deg
say

say "[2/2] 计数轮（带 strace）：每响应的写系统调用次数"
count_run "$BIN_OPT" opt
count_run "$BIN_DEG" deg
say

# ---------------------------------------------------------------------------
#  3. 汇总
# ---------------------------------------------------------------------------
qps_opt=$(median < "$TMP/qps.opt"); qps_deg=$(median < "$TMP/qps.deg")
cpu_opt=$(median < "$TMP/cpu.opt"); cpu_deg=$(median < "$TMP/cpu.deg")

# 计数轮解析失败时刻意不写 cnt.$tag（宁可显式 n/a，也不要假数字），
# 所以这里必须先判存在 —— 直接 read 一个不存在的文件，bash 会往 stderr
# 打一行 "No such file or directory"，看着像脚本崩了。
# cnt 行格式（8 列，位置固定）：
#   reqs  套接字writev  套接字write  eventfd写  日志文件写  其他写  原始writev  原始write
reqs_o=0; swv_o=0; sw_o=0; ev_o=0; fw_o=0; ow_o=0; rwv_o=0; rw_o=0
reqs_d=0; swv_d=0; sw_d=0; ev_d=0; fw_d=0; ow_d=0; rwv_d=0; rw_d=0
[ -f "$TMP/cnt.opt" ] && read -r reqs_o swv_o sw_o ev_o fw_o ow_o rwv_o rw_o < "$TMP/cnt.opt"
[ -f "$TMP/cnt.deg" ] && read -r reqs_d swv_d sw_d ev_d fw_d ow_d rwv_d rw_d < "$TMP/cnt.deg"
reqs_o=${reqs_o:-0}; swv_o=${swv_o:-0}; sw_o=${sw_o:-0}
reqs_d=${reqs_d:-0}; swv_d=${swv_d:-0}; sw_d=${sw_d:-0}

# 每响应的写系统调用次数 —— **只数连接套接字上的**（write 与 writev 都算一次写系统调用）。
# 唤醒 eventfd、日志文件这些不参与"发响应"，必须排除，否则两版都会各自虚高约 1 次/响应。
per_o=$(awk -v r="$reqs_o" -v a="$swv_o" -v b="$sw_o" 'BEGIN{ if (r+0>0) printf "%.3f", (a+b)/r; else print "n/a" }')
per_d=$(awk -v r="$reqs_d" -v a="$swv_d" -v b="$sw_d" 'BEGIN{ if (r+0>0) printf "%.3f", (a+b)/r; else print "n/a" }')

# 不区分 fd 的原始口径（进程里所有 write），只为让"数字为什么变了"一眼可见。
per_raw_o=$(awk -v r="$reqs_o" -v a="${rwv_o:-0}" -v b="${rw_o:-0}" 'BEGIN{ if (r+0>0) printf "%.3f", (a+b)/r; else print "n/a" }')
per_raw_d=$(awk -v r="$reqs_d" -v a="${rwv_d:-0}" -v b="${rw_d:-0}" 'BEGIN{ if (r+0>0) printf "%.3f", (a+b)/r; else print "n/a" }')

# 中文标签的显示宽度是 ASCII 的两倍，用 printf 的 %-Ns 补空格会对不齐
# （它按字节数补，中文一个占 3 字节）。所以不去硬凑表格，改成键值输出。
[ "$cpu_opt" = "NA" ] && cpu_opt="n/a（本环境读不到 /proc/<pid>/stat）"
[ "$cpu_deg" = "NA" ] && cpu_deg="n/a（本环境读不到 /proc/<pid>/stat）"

show_variant() {   # $1=名称 $2=QPS中位 $3=每响应CPU $4=套接字writev $5=套接字write
                   # $6=每响应写调用 $7=其他fd写总次数 $8=未过滤fd口径的每响应
    say "$1"
    say "    QPS(参考)        : $2"
    say "    每响应 CPU       : $3"
    say "    ★ 每响应写调用   : $6   （连接套接字：writev $4 + write $5）"
    say "      其他 fd 上的写 : $7 次（eventfd 唤醒 / 日志文件，不计入）"
    say "      未过滤 fd 口径 : ${8:-n/a} 次/响应（含上面那些，仅备对照）"
}

say "汇总（性能轮取中位；计数轮为确定性计数，重复跑不变）"
rule
show_variant "优化版（一次 writev）" "${qps_opt:-NA}" "$cpu_opt" \
    "$swv_o" "$sw_o" "$per_o" "$(( ev_o + fw_o + ow_o ))" "$per_raw_o"
say
show_variant "退化版（逐段 write）" "${qps_deg:-NA}" "$cpu_deg" \
    "$swv_d" "$sw_d" "$per_d" "$(( ev_d + fw_d + ow_d ))" "$per_raw_d"
rule
say

# 结论：只有两个比例都有数才算得出来
awk -v po="$per_o" -v pd="$per_d" -v co="$cpu_opt" -v cd="$cpu_deg" '
BEGIN {
    if (po != "n/a" && pd != "n/a" && pd + 0 > 0) {
        printf "写系统调用：每响应 %.2f 次 -> %.2f 次，减少 %.0f%%\n", pd, po, (pd - po) * 100 / pd;
    } else {
        print "写系统调用：数据不足，无法计算（看原始输出）";
    }
    if (co != "" && cd != "" && cd + 0 > 0) {
        printf "每响应 CPU 时间：%.2f µs -> %.2f µs，变化 %.0f%%\n", cd, co, (cd - co) * 100 / cd;
    } else {
        print "每响应 CPU 时间：本环境读不到 /proc/<pid>/stat（非 Linux），跳过";
    }
}'

# 断崖式下跌的诊断：退化版慢一个数量级以上，绝不是"多一次系统调用"能解释的。
# 头号嫌疑是 Nagle + delayed-ACK（前提是连接没设 TCP_NODELAY —— 设了就没这现象）。
# 判据：延迟跳到一个 ~40ms 的平台，而且进程 CPU 占用很低（在等，不在算）。
if [ "${qps_opt:-NA}" != "NA" ] && [ "${qps_deg:-NA}" != "NA" ]; then
    awk -v o="$qps_opt" -v d="$qps_deg" 'BEGIN{
        if (o + 0 > 0 && d + 0 > 0 && d * 10 < o) {
            printf "\n⚠️  退化版吞吐只有优化版的 %.1f%% —— 这个量级不是多一次系统调用能造成的。\n", d * 100 / o;
            print  "    先查连接有没有设 TCP_NODELAY：分片写会踩 Nagle + delayed-ACK，";
            print  "    典型症状是「每请求延迟跳上 ~40ms 的平台、而进程 CPU 占用很低（在等，不在算）」。";
            print  "    本项目 acceptor.cpp 的 setTcpNoDelay() 就是为此而设。";
        }
    }'
fi

say
say "怎么读这张表 / 怎么用这组数字："
say "  · **结论只引「★ 每响应写调用」那一行**。它是确定性的：每响应一次 writev 就是 1.000，"
say "    退回逐段 write 就是 2.000，重复跑不会变，也没有噪声。"
say "  · 口径是「**连接套接字上的写**」（strace -yy 按 fd 类型过滤）。进程里还有别的 write，"
say "    但与「发响应」无关：协程总是在 doPendingFunctors() 里被恢复，于是协程函数体的整个"
say "    执行窗口内 callingPendingFunctors_ 为真；而响应又恰好在这同一窗口内同步写完，"
say "    于是写完成路径里那次 queueInLoop（tcp_connection.cpp:278）命中唤醒分支"
say "    （event_loop.cpp:132），每响应多一次 ::write(eventfd, &one, 8)。外加异步日志落盘。"
say "    不排除它们，两版会各自虚高约 1 次/响应 ——"
say "    不按 fd 过滤时实测就是 2.011 vs 3.006；那 1 次是两版共有的额外开销，不是 writev 的功劳。"
say "  · QPS 列只是参考，本项目单次抖动 10%~30%（笔记本功耗墙），**不要用 QPS 的差值下结论**。"
say "  · 每响应 CPU 这一列只是换算：服务端被压满时 CPUΔ 两版都 ≈ 墙钟 × IO 线程数，"
say "    于是该列 ≈ 常数 ÷ QPS —— **与 QPS 同源，不算独立证据**。"
say "原始输出已写入：$SRV_DIR/$OUT"
