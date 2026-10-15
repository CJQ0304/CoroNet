#!/usr/bin/env bash
# ============================================================================
#  CoroNet 扩展性压测（一键跑完）
#
#  目的：扫描 0 / 1 / 2 / 4 / 8 个 IO 线程，产出「线程数 → 吞吐 / 延迟」的扩展性曲线，
#  量化「加线程到底值不值」，并找出拐点。
#
#  用法（在 Linux / WSL 里跑；服务端目录别放在 /mnt/* 这类挂载盘上）：
#      cd server && cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j
#      cd .. && SRV_DIR="$(pwd)/server" bash bench-scaling.sh
#
#  可调环境变量：
#      THREADS_LIST="0 1 2 4 8"   要测的服务端 IO 线程数（默认）
#      REPEAT=3                   每个配置跑几次，取中位数（默认 3）
#      DURATION=10s               每次压测时长（默认 10s）
#      CLIENT_T=4                 客户端线程数 —— **保持固定**，只让服务端变
#      CLIENT_C=100               客户端连接数
#      PORT=8080
#      SRV_DIR=<服务端目录>        （默认 $HOME/projects/CoroNet/server）
#
#  产出：终端打印一张表 + 原始输出落到 scaling-<时间戳>.log
#
#  四条口径要求（脚本已按此实现）：
#    1. **只取中位数**。单次 10s 的抖动可达 10%~30%（本项目实测三组数据全都单调递减，
#       是笔记本 PL2→PL1 功耗墙导致的系统性漂移），引用单次最好成绩就是虚高。
#    2. **配置顺序必须逐轮轮转**。若固定按 0→1→2→4→8 升序跑，排最后的配置永远在
#       "机器最热/最累"的时刻被测，一旦有漂移就会被系统性冤枉。
#    3. **客户端线程数固定**。客户端和服务端在同一台机器上抢 CPU，
#       两边一起加线程是"加压"不是"扩容"，会把扩展性曲线测歪。
#    4. **别在 /mnt/* 这类挂载盘下跑**。跨盘 IO 慢且数据失真（脚本会检测并警告）。
# ============================================================================
set -u

SRV_DIR="${SRV_DIR:-$HOME/projects/CoroNet/server}"
PORT="${PORT:-8080}"
THREADS_LIST="${THREADS_LIST:-0 1 2 4 8}"
REPEAT="${REPEAT:-3}"
DURATION="${DURATION:-10s}"
CLIENT_T="${CLIENT_T:-4}"
CLIENT_C="${CLIENT_C:-100}"
URL="http://127.0.0.1:${PORT}/plain"
OUT="${OUT:-scaling-$(date +%Y%m%d-%H%M%S).log}"
BIN="$SRV_DIR/build/http_server"

say()  { printf '%s\n' "$*"; }
die()  { printf '错误：%s\n' "$*" >&2; exit 1; }
rule() { printf '%s\n' "--------------------------------------------------------------------"; }

# ---------------------------------------------------------------------------
#  小工具
# ---------------------------------------------------------------------------

# wrk 的延迟带单位（70.00us / 1.10ms / 1.50s），统一折算成 µs 才能排序取中位。
# 注意判断顺序：'us' 必须在 's' 之前判断，否则 70.00us 会被 's$' 先匹配走。
to_us() {
    awk -v x="$1" 'BEGIN{
        if      (x ~ /us$/) { sub(/us$/,"",x); printf "%.1f", x        }
        else if (x ~ /ms$/) { sub(/ms$/,"",x); printf "%.1f", x * 1000 }
        else if (x ~ /s$/)  { sub(/s$/,"",x);  printf "%.1f", x * 1000000 }
        else                { printf "%.1f", x }
    }'
}

# 读 stdin 的一列数，输出中位数（偶数个取中间两个的平均）
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

# 把列表循环左移 $1 位。用于**逐轮轮转配置顺序**：
# 固定升序跑时，最后一个配置永远在"机器最热/最累"的时刻被测，会被系统性冤枉。
# 第 r 轮左移 r-1 位，于是每个配置在每一轮落到的位置都不同。
rotate() {
    local k="$1"; shift
    local arr=("$@")
    local n=${#arr[@]}
    [ "$n" -le 0 ] && return 0
    local i
    for (( i = 0; i < n; i++ )); do
        printf '%s ' "${arr[$(( (i + k) % n ))]}"
    done
}

# 等端口进入 LISTEN，最多等 $2 秒；优先用 ss，没有就退回 bash 的 /dev/tcp
wait_listen() {
    local port="$1" limit="$2" i=0
    local tries=$(( limit * 10 ))
    while [ "$i" -lt "$tries" ]; do
        if command -v ss >/dev/null 2>&1; then
            ss -ltn 2>/dev/null | grep -q ":${port}[[:space:]]" && return 0
        else
            (exec 3<>"/dev/tcp/127.0.0.1/${port}") 2>/dev/null && { exec 3>&-; return 0; }
        fi
        i=$(( i + 1 ))
        sleep 0.1
    done
    return 1
}

# 精确按进程名杀，避免误伤；失败不算错（本来就没在跑）
kill_server() {
    pkill -x http_server 2>/dev/null || true
    sleep 0.3
}

SRV_PID=""
cleanup() {
    [ -n "$SRV_PID" ] && kill "$SRV_PID" 2>/dev/null || true
    [ -n "${TMP:-}" ] && rm -rf "$TMP"
    return 0
}
trap cleanup EXIT INT TERM

# ---------------------------------------------------------------------------
#  0. 前置检查
# ---------------------------------------------------------------------------
[ -d "$SRV_DIR" ] || die "找不到服务端目录：$SRV_DIR（可用 SRV_DIR=... 指定）"
[ -x "$BIN" ]     || die "找不到可执行文件：$BIN
     先编译：cd $SRV_DIR && cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j"
command -v wrk >/dev/null 2>&1 || die "没有 wrk：sudo apt install -y wrk"

case "$SRV_DIR" in
    /mnt/*) say "⚠️  警告：服务端在 $SRV_DIR（/mnt 下），跨盘 IO 会让压测数据失真。"
            say "    建议换到 \$HOME/projects/CoroNet/server 下跑。" ;;
esac

TMP="$(mktemp -d)"
cd "$SRV_DIR" || die "无法进入 $SRV_DIR"

say "================================================================"
say " CoroNet 扩展性压测"
say "================================================================"
say " 服务端   : $BIN"
say " 客户端   : wrk -t$CLIENT_T -c$CLIENT_C -d$DURATION --latency（固定不变）"
say " 线程数   : $THREADS_LIST    每个跑 $REPEAT 次取中位"
say " 原始输出 : $SRV_DIR/$OUT"
say "================================================================"
say

# ---------------------------------------------------------------------------
#  1. 环境信息（顺手把 README「测试环境」表里缺的内存补上）
# ---------------------------------------------------------------------------
say "[0/2] 环境信息"
say "  CPU      : $(lscpu 2>/dev/null | awk -F': +' '/Model name/{print $2; exit}')  /  nproc = $(nproc 2>/dev/null)"
say "  内存     : $(free -h 2>/dev/null | awk '/^Mem:/{print $2" total"}')"
say "  系统     : $( (lsb_release -ds 2>/dev/null) || (uname -s) )  /  内核 $(uname -r)"
say "  编译器   : $( (g++ --version 2>/dev/null | head -1) || echo '?')"
say "  压测工具 : $(wrk --version 2>&1 | head -1)"
say "  机器负载 : $(cat /proc/loadavg 2>/dev/null)"
say "  ulimit -n: $(ulimit -n)   （需要 > 客户端连接数 $CLIENT_C）"
say

{
    echo "### CoroNet 扩展性压测 $(date '+%F %T')"
    echo "cpu=$(lscpu 2>/dev/null | awk -F': +' '/Model name/{print $2; exit}')"
    echo "nproc=$(nproc 2>/dev/null)"
    echo "mem=$(free -h 2>/dev/null | awk '/^Mem:/{print $2}')"
    echo "kernel=$(uname -r)"
    echo "gxx=$(g++ --version 2>/dev/null | head -1)"
    echo "wrk=$(wrk --version 2>&1 | head -1)"
    echo "client=-t$CLIENT_T -c$CLIENT_C -d$DURATION"
    # 注意：这里用单引号 + 「」，不要用 ASCII 双引号嵌套 ——
    # `echo "a"b"c"` 会静默变成 abc（引号被吃掉），不报错但内容不对。
    echo 'order=逐轮轮转（第 r 轮左移 r-1 位），避免把「排最后」的配置冤枉成「更慢」'
} >> "$OUT"

# ---------------------------------------------------------------------------
#  2. 逐个线程数压测
#
#  结构说明：外层是「轮」，内层是该轮的配置顺序。
#  - 逐轮轮转（见 rotate 注释）：把"测试时刻"这个混淆变量与"配置"解耦；
#  - 代价是服务端重启次数从 N 次变成 N×REPEAT 次（每次约 2.5s，可接受），
#    好处是**每个测量点都跑在刚启动的干净进程上**，不受前一轮的进程状态影响。
# ---------------------------------------------------------------------------
ORDER=($THREADS_LIST)
NCONF=${#ORDER[@]}
TOTAL=$(( NCONF * REPEAT ))
DONE=0

# 清空上一轮可能留下的中间文件（注意：只在这里清一次，轮内必须用 >> 追加）
for n in $THREADS_LIST; do
    : > "$TMP/qps.$n"; : > "$TMP/p50.$n"; : > "$TMP/p99.$n"
done

say "[1/2] 开始压测（每个配置 $REPEAT 次 × $DURATION，配置顺序逐轮轮转）"
say

for r in $(seq 1 "$REPEAT"); do
    round="$(rotate $(( r - 1 )) "${ORDER[@]}")"
    say "  ── 第 $r / $REPEAT 轮，顺序：$round"

    for n in $round; do
        DONE=$(( DONE + 1 ))
        kill_server

        CORONET_LOG_QUIET=1 "$BIN" "$PORT" "$n" >/dev/null 2>&1 &
        SRV_PID=$!

        if ! wait_listen "$PORT" 15; then
            say "  [$DONE/$TOTAL] rep=$r threads=$n   ✗ 服务端 15 秒内没监听 $PORT，跳过该点"
            kill_server; SRV_PID=""
            continue
        fi
        sleep 1     # 让各 IO 线程把 epoll/eventfd/timerfd 都建好再压

        # 服务端自己打的这行是"线程数确实生效了"的凭据，留进原始日志
        started=$(ls -t coronet-http.*.log 2>/dev/null | head -1)
        if [ -n "$started" ]; then
            grep "IO threads" "$started" 2>/dev/null | tail -1 >> "$OUT"
        fi

        res="$(wrk -t"$CLIENT_T" -c"$CLIENT_C" -d"$DURATION" --latency "$URL" 2>&1)"
        printf '\n----- rep=%s/%s  threads=%s -----\n%s\n' "$r" "$REPEAT" "$n" "$res" >> "$OUT"

        qps=$(printf '%s\n' "$res" | awk '/Requests\/sec/{print $2; exit}')
        p50=$(printf '%s\n' "$res" | awk '/^[[:space:]]*50%/{print $2; exit}')
        p99=$(printf '%s\n' "$res" | awk '/^[[:space:]]*99%/{print $2; exit}')

        kill_server; SRV_PID=""

        if [ -z "$qps" ]; then
            say "  [$DONE/$TOTAL] rep=$r threads=$n   ✗ wrk 没给出 Requests/sec（看 $OUT）"
            continue
        fi
        printf '%s\n' "$qps"            >> "$TMP/qps.$n"
        printf '%s\n' "$(to_us "$p50")" >> "$TMP/p50.$n"
        printf '%s\n' "$(to_us "$p99")" >> "$TMP/p99.$n"
        say "  [$DONE/$TOTAL] rep=$r/$REPEAT threads=$n   QPS=$qps  P50=$p50  P99=$p99"
    done
    say
done

# ---------------------------------------------------------------------------
#  3. 汇总表
# ---------------------------------------------------------------------------
# 基准取「1 个 worker loop」。注意线程 0 与 1 是**同一个并行度**：
#   0 → getNextLoop() 返回 baseLoop（全部连接在 baseLoop 上处理）
#   1 → 只有一个 IO 线程（全部连接都在它上面处理）
# 两者都只有 1 个 loop 干活，所以这两行**应当几乎相等** —— 这是本轮测量的自检。
# 优先拿 `1` 当基准（0 是退化配置，1 才是最小可用配置）；列表里没有 1 才退回 0。
BASE_T=""
for n in $THREADS_LIST; do [ "$n" -eq 1 ] && { BASE_T="$n"; break; }; done
if [ -z "$BASE_T" ]; then
    for n in $THREADS_LIST; do [ "$n" -eq 0 ] && { BASE_T="$n"; break; }; done
fi
[ -n "$BASE_T" ] || BASE_T="${ORDER[0]}"
BASE=$(median < "$TMP/qps.$BASE_T" 2>/dev/null || echo 0)

say "[2/2] 汇总（中位数；配置顺序逐轮轮转以抵消功耗墙漂移；基准 = 1 个 worker loop）"
printf ' %-5s | %-11s | %-11s | %-9s | %-9s | %-9s | %-11s | %s\n' \
       "线程" "worker loop" "QPS(中位)" "P50(µs)" "P99(µs)" "相对基准" "每loop QPS" "每loop 效率"
rule

for n in $THREADS_LIST; do
    q=$(median < "$TMP/qps.$n")
    p50=$(median < "$TMP/p50.$n")
    p99=$(median < "$TMP/p99.$n")
    [ "$q" = "NA" ] && { printf ' %-5s | %s\n' "$n" "（本配置没有有效数据）"; continue; }

    loops=$(( n > 1 ? n : 1 ))
    awk -v n="$n" -v L="$loops" -v q="$q" -v p50="$p50" -v p99="$p99" -v b="$BASE" 'BEGIN{
        per = q / L
        if (b + 0 > 0) {
            sp    = q / b
            eff   = per * 100 / b
            spstr = sprintf("%.2fx", sp)
            printf " %-5s | %-11s | %-11.0f | %-9s | %-9s | %-9s | %-11.0f | %.0f%%\n",
                   n, L, q, p50, p99, spstr, per, eff
        } else {
            printf " %-5s | %-11s | %-11.0f | %-9s | %-9s | %-9s | %-11.0f | %s\n",
                   n, L, q, p50, p99, "-", per, "-"
        }
    }'
done
rule
say
say "怎么读这张表："
say "  · 线程 0 与 1 是**同一个并行度**（都只有 1 个 loop 在处理连接：0 = 全在 baseLoop，"
say "    1 = 全在那一个 IO 线程）→ 两行应当几乎相等，这是本轮测量的内部一致性检查。"
say "  · 「相对基准」= 本配置 QPS ÷ 1 个 loop 的 QPS。理想线性扩展时 2/4/8 应为 2/4/8 倍；"
say "    次线性是正常的，会被两个结构性因素吃掉："
say "      ① 客户端与被压端同机、且客户端线程数固定（这里 -t$CLIENT_T），客户端自己也会先到顶。"
say "         想验证：同并发下把客户端线程数加倍（-t$((CLIENT_T * 2)) -c$CLIENT_C），看 QPS 有没有涨。"
say "      ② 回环（loopback）收发两侧都要烧 CPU，约等于真实网络的两倍成本。"
say "  · 「每 loop 效率」跌破 80% 就说明再加 loop 不太划算 —— 但拐点会随**客户端并发**变化，"
say "    所以报数字必须带上客户端参数（同一个服务端线程数，换个 -c 结论可能相反）。"
say "  · 顺序已逐轮轮转，但单次抖动仍可达 10%~30%（笔记本 PL2→PL1 功耗墙），"
say "    所以只看中位数，别引用某一次的峰值。"
say
say "原始输出已写入：$SRV_DIR/$OUT"
