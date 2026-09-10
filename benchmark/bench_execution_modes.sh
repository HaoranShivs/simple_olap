#!/usr/bin/env bash
#
# 对比「整条 SQL 语句」在单线程 / 多线程两种执行模式下的耗时。
#
# 思路：
#   复用 REPL 二进制（simple_olap），通过 --mode single|multi 切换执行模式，
#   对同一条 SQL 分别重复计时，输出 best / median / mean 与相对加速比。
#   开启 --silent 跳过逐行格式化，避免结果打印（I/O）主导计时。
#   数据集在独立数据库目录内生成（默认 benchmark/bench_data/modes_db），
#   不污染项目主库 database/。
#
# 用法：
#   ./benchmark/bench_execution_modes.sh
#   ./benchmark/bench_execution_modes.sh --rows 1000000 --batch 4096 --queue 32
#   ./benchmark/bench_execution_modes.sh --threads 8 --repeats 7 --warmup 3
#   ./benchmark/bench_execution_modes.sh \
#       --sql "SELECT COUNT(*) FROM bench_data WHERE value > 0.5;"
#   ./benchmark/bench_execution_modes.sh --no-gen --db "$(pwd)/database" \
#       --sql "SELECT id, price FROM goods WHERE price > 500;"
#   ./benchmark/bench_execution_modes.sh --no-build
#
# 参数（默认值与 benchmark/bench_parallel_scan.cpp 对齐）：
#   --rows N        数据集行数（默认 4000000）
#   --batch B       写入侧 DataChunk 行数（默认 8192）
#   --queue Q       并行批队列容量（默认 16）
#   --threads N     多线程模式的 worker 数（默认 = nproc）
#   --repeats R     每个模式计时轮数（默认 5）
#   --warmup W      每轮计时前的预热次数（默认 2）
#   --table NAME    生成的数据集表名（默认 bench_data）
#   --db DIR        数据库根目录（默认 <repo>/benchmark/bench_data/modes_db）
#   --sql "..."     追加一条待测 SQL（可多次；必须以 ';' 结尾）
#   --no-gen        跳过数据集生成，直接使用 --db 中已存在的表
#   --bin PATH      指定 simple_olap 二进制路径
#   --no-build      跳过 cmake 构建，直接使用已有二进制
#   -h, --help      显示帮助
#
# 注意：
#   - 计时包含进程启动 + Catalog/表打开等固定开销，脚本会单独测量「仅 exit」
#     的基线耗时供参考；真正可比的是同一条 SQL 下 single 与 multi 的相对关系。
#   - 默认数据集列为 (id INT64, value DOUBLE)，value ~ U(0, 1)；脚本只做只读查询。

set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${ROOT_DIR}/build"
BUILD_TYPE="${BUILD_TYPE:-Release}"
JOBS="$(nproc 2>/dev/null || echo 4)"

# ---------- 可调参数（与 bench_parallel_scan 对齐） ----------
ROWS=4000000
BATCH=8192
QUEUE=16
TABLE="bench_data"
THREADS="$(nproc 2>/dev/null || echo 4)"
REPEATS=5
WARMUP=2
DB_DIR="${ROOT_DIR}/benchmark/bench_data/modes_db"

DO_BUILD=1
DO_GEN=1
BIN_OVERRIDE=""
declare -a QUERIES=()

usage() {
    cat <<'EOF'
对比「整条 SQL 语句」在单线程 / 多线程两种执行模式下的耗时。

用法：
  ./benchmark/bench_execution_modes.sh [options]

选项：
  --rows N        数据集行数（默认 4000000）
  --batch B       写入侧 DataChunk 行数（默认 8192）
  --queue Q       并行批队列容量（默认 16）
  --threads N     多线程模式的 worker 数（默认 = nproc）
  --repeats R     每个模式计时轮数（默认 5）
  --warmup W      每轮计时前的预热次数（默认 2）
  --table NAME    生成的数据集表名（默认 bench_data）
  --db DIR        数据库根目录（默认 <repo>/benchmark/bench_data/modes_db）
  --sql "..."     追加一条待测 SQL（可多次；必须以 ';' 结尾）
  --no-gen        跳过数据集生成，直接使用 --db 中已存在的表
  --bin PATH      指定 simple_olap 二进制路径
  --no-build      跳过 cmake 构建，直接使用已有二进制
  -h, --help      显示帮助
EOF
    exit 0
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --rows)    ROWS="$2"; shift 2 ;;
        --batch)   BATCH="$2"; shift 2 ;;
        --queue)   QUEUE="$2"; shift 2 ;;
        --threads) THREADS="$2"; shift 2 ;;
        --repeats) REPEATS="$2"; shift 2 ;;
        --warmup)  WARMUP="$2"; shift 2 ;;
        --table)   TABLE="$2"; shift 2 ;;
        --db)      DB_DIR="$2"; shift 2 ;;
        --sql)     QUERIES+=("$2"); shift 2 ;;
        --no-gen)  DO_GEN=0; shift ;;
        --bin)     BIN_OVERRIDE="$2"; shift 2 ;;
        --no-build) DO_BUILD=0; shift ;;
        -h|--help) usage ;;
        *) echo "unknown argument: $1" >&2; exit 1 ;;
    esac
done

# 默认查询负载：全表扫描 / 过滤 / 过滤后聚合（针对生成的数据集）
if [[ ${#QUERIES[@]} -eq 0 ]]; then
    QUERIES+=(
        "SELECT * FROM ${TABLE};"
        "SELECT id, value FROM ${TABLE} WHERE value > 0.5;"
        "SELECT COUNT(*) FROM ${TABLE} WHERE value > 0.5;"
    )
fi

# 规范：确保每条 SQL 以 ';' 结尾（REPL 以 ';' 判断语句结束）
normalized=()
for q in "${QUERIES[@]}"; do
    if [[ "${q}" != *";" ]]; then
        q="${q};"
    fi
    normalized+=("${q}")
done
QUERIES=("${normalized[@]}")

if [[ "${DO_BUILD}" -eq 1 ]]; then
    echo "[1/3] configure + build (${BUILD_TYPE}) ..."
    cmake -S "${ROOT_DIR}" -B "${BUILD_DIR}" -DCMAKE_BUILD_TYPE="${BUILD_TYPE}" >/dev/null
    cmake --build "${BUILD_DIR}" -j"${JOBS}" --target simple_olap >/dev/null
fi

BIN="${BIN_OVERRIDE:-${BUILD_DIR}/bin/simple_olap}"
if [[ ! -x "${BIN}" ]]; then
    echo "binary not found: ${BIN}" >&2
    echo "先构建：cmake --build build --target simple_olap" >&2
    exit 1
fi

# ---------- 数据集生成（独立数据库目录，不污染主库） ----------
if [[ "${DO_GEN}" -eq 1 ]]; then
    echo "[2/3] generate dataset: rows=${ROWS} batch=${BATCH} -> ${DB_DIR}"
    rm -rf "${DB_DIR}"
    mkdir -p "$(dirname "${DB_DIR}")"
    "${BIN}" --db "${DB_DIR}" --gen-table "${TABLE}" --rows "${ROWS}" --batch "${BATCH}"
else
    echo "[2/3] skip dataset generation (--no-gen), using ${DB_DIR}"
fi

# 传给每次运行的公共参数：数据库目录 / worker 数 / 队列容量 / 静默
RUN_FLAGS=(--db "${DB_DIR}" --threads "${THREADS}" --queue "${QUEUE}" --silent)

ns_now() { date +%s%N; }

# 执行一条 SQL，返回本次运行的纳秒耗时
time_once() {
    local mode="$1" sql="$2" start end
    start="$(ns_now)"
    printf '%s\nexit\n' "${sql}" | "${BIN}" --mode "${mode}" "${RUN_FLAGS[@]}" >/dev/null 2>&1
    end="$(ns_now)"
    echo "$(( end - start ))"
}

# 仅启动、不执行任何查询的纳秒耗时（进程 + 打开 database 的固定开销）
time_startup_once() {
    local start end
    start="$(ns_now)"
    printf 'exit\n' | "${BIN}" --mode single "${RUN_FLAGS[@]}" >/dev/null 2>&1
    end="$(ns_now)"
    echo "$(( end - start ))"
}

# 从 stdin 读取 ns 整数序列，输出 "min median mean"（ns）
compute_stats() {
    sort -n | awk '
        { a[NR] = $1; sum += $1 }
        END {
            n = NR
            if (n == 0) { print "0 0 0"; exit }
            med = (n % 2) ? a[(n + 1) / 2] : (a[n / 2] + a[n / 2 + 1]) / 2
            printf "%d %.0f %.0f\n", a[1], med, sum / n
        }'
}

ms() { awk -v v="$1" 'BEGIN { printf "%.2f", v / 1000000 }'; }

rows_of() {
    local mode="$1" sql="$2" out
    out="$(printf '%s\nexit\n' "${sql}" \
        | "${BIN}" --mode "${mode}" "${RUN_FLAGS[@]}" 2>/dev/null \
        | sed -n 's/.*(\([0-9][0-9]*\) rows).*/\1/p' | tail -n 1)"
    echo "${out:-0}"
}

printf '\n=== 单线程 vs 多线程：整条 SQL 执行耗时对比 ===\n'
printf 'binary    : %s\n' "${BIN}"
printf 'db        : %s\n' "${DB_DIR}"
if [[ "${DO_GEN}" -eq 1 ]]; then
    printf 'dataset   : %s (generated: rows=%s batch=%s)\n' "${TABLE}" "${ROWS}" "${BATCH}"
else
    printf 'dataset   : %s (reused, --no-gen)\n' "${TABLE}"
fi
printf 'threads   : %s (queue=%s)\n' "${THREADS}" "${QUEUE}"
printf 'repeats   : %s (+%s warmup)\n' "${REPEATS}" "${WARMUP}"

# 启动基线
startup_samples=""
for (( w = 0; w < WARMUP; ++w )); do time_startup_once >/dev/null; done
for (( r = 0; r < REPEATS; ++r )); do
    startup_samples+="$(time_startup_once)"$'\n'
done
read -r su_min su_med su_avg <<<"$(printf '%s' "${startup_samples}" | compute_stats)"
printf 'startup   : median %s ms (仅启动+打开库，供扣除固定开销)\n\n' "$(ms "${su_med}")"

declare -a SUMMARY=()
idx=0

# Summary 表列宽：按最长 SQL 自适应
WIDTH=3
for q in "${QUERIES[@]}"; do
    (( ${#q} > WIDTH )) && WIDTH=${#q}
done

for sql in "${QUERIES[@]}"; do
    idx=$(( idx + 1 ))
    echo "[${idx}] ${sql}"

    row_single="$(rows_of single "${sql}")"
    row_multi="$(rows_of multi "${sql}")"
    if [[ "${row_single}" == "${row_multi}" ]]; then
        printf '    rows    : single=%s multi=%s  [OK]\n' "${row_single}" "${row_multi}"
    else
        printf '    rows    : single=%s multi=%s  [MISMATCH]\n' "${row_single}" "${row_multi}"
    fi

    declare -A med_of=()
    for mode in single multi; do
        for (( w = 0; w < WARMUP; ++w )); do time_once "${mode}" "${sql}" >/dev/null; done

        samples=""
        for (( r = 0; r < REPEATS; ++r )); do
            samples+="$(time_once "${mode}" "${sql}")"$'\n'
        done

        read -r mn md avg <<<"$(printf '%s' "${samples}" | compute_stats)"
        med_of["${mode}"]="${md}"

        printf '    %-6s  : min %s ms | median %s ms | mean %s ms\n' \
            "${mode}" "$(ms "${mn}")" "$(ms "${md}")" "$(ms "${avg}")"
    done

    sp="$(awk -v s="${med_of[single]}" -v m="${med_of[multi]}" \
        'BEGIN { if (m > 0) printf "%.2f", s / m; else printf "0.00" }')"
    printf '    speedup : %sx (median single / median multi)\n\n' "${sp}"

    SUMMARY+=("$(printf '%-*s %11s %11s %10s' "${WIDTH}" "${sql}" \
        "$(ms "${med_of[single]}")" "$(ms "${med_of[multi]}")" "${sp}x")")
done

echo "=== Summary (median, ms) ==="
printf '%-*s %11s %11s %10s\n' "${WIDTH}" "SQL" "single" "multi" "speedup"
printf '%-*s %11s %11s %10s\n' "${WIDTH}" \
    "$(printf '%*s' "${WIDTH}" '' | tr ' ' '-')" "-----------" "-----------" "----------"
for line in "${SUMMARY[@]}"; do
    echo "${line}"
done
echo
echo "提示：multi 的加速比受消费端（结果收集/深拷贝，单线程）与计划形状影响；"
echo "      纯扫描/投影的加速有限，过滤与聚合等 CPU 密集场景收益更明显。"
echo "      可调参数：--rows / --batch / --queue / --threads / --repeats / --warmup。"
