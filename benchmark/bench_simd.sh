#!/usr/bin/env bash
#
# 在同一个二进制上对比「SIMD (AVX2)」与「非 SIMD (scalar)」两种后端下整条 SQL 的耗时。
#
# 思路：
#   SIMD 后端由 KernelRegistry 在进程启动时按 CPU 特性自动选择，没有编译期开关。
#   本脚本通过环境变量 SIMPLE_OLAP_FORCE_SCALAR=1 强制回落到 scalar backend，
#   于是可以在同一个 simple_olap 二进制上做 A/B 对比，避免两次构建引入差异。
#
#   热路径是 storage 谓词下推里的 typed compare kernel（见 src/simd/）：
#     WHERE <col> > <const>               -> 生成 1 个 SelectionMask
#     WHERE <c1> > <v1> AND <c2> < <v2>   -> 生成 2 个 SelectionMask 后 AND
#   scalar 与 AVX2 backend 共用同一套 SelectionMask 接口与 AND 合并模型，
#   因此两者结果必须一致（脚本会逐条 SQL 校验行数）。
#
#   开启 --silent 只打印 (N rows)，避免结果格式化 I/O 主导计时。
#   数据集在独立数据库目录内生成（默认 benchmark/bench_data/modes_db），
#   不污染项目主库 database/。
#
#   默认使用 --mode single（单线程）：这样唯一随后端变化的开销就是 compare kernel，
#   信号最干净、可复现。改成 --mode multi 会在多线程并行扫描下测量，更贴近生产，
#   但线程调度/结果收集的噪声会明显稀释 SIMD 差异（甚至出现个别查询倒挂）。
#
# 用法：
#   ./benchmark/bench_simd.sh
#   ./benchmark/bench_simd.sh --rows 8000000 --repeats 7 --warmup 3
#   ./benchmark/bench_simd.sh --mode multi --threads 8
#   ./benchmark/bench_simd.sh \
#       --sql "SELECT COUNT(*) FROM bench_data WHERE value > 0.5;"
#   ./benchmark/bench_simd.sh --no-gen --bin build/bin/simple_olap
#   ./benchmark/bench_simd.sh --no-build
#
# 参数（默认值与 benchmark/bench_parallel_scan.cpp 对齐）：
#   --rows N       数据集行数（默认 4000000）
#   --batch B      写入侧 DataChunk 行数（默认 8192）
#   --queue Q      并行批队列容量（默认 16）
#   --threads N    worker 数（默认 = nproc；仅 --mode multi 生效）
#   --repeats R    每个后端的计时轮数（默认 5）
#   --warmup W     每个后端计时前的预热次数（默认 2）
#   --table NAME   生成的数据集表名（默认 bench_data）
#   --db DIR       数据库根目录（默认 <repo>/benchmark/bench_data/modes_db）
#   --sql "..."    追加一条待测 SQL（可多次；必须以 ';' 结尾）
#   --mode M       执行模式 single|multi（额外参数，默认 single）
#   --no-gen       跳过数据集生成，直接使用 --db 中已存在的表
#   --bin PATH     指定 simple_olap 二进制路径
#   --no-build     跳过 cmake 构建，直接使用已有二进制
#   -h, --help     显示帮助
#
# 注意：
#   - 计时包含进程启动 + Catalog/表打开等固定开销，脚本会分别测量两个后端的
#     「仅 exit」基线耗时供参考；真正可比的是同一条 SQL 下 scalar 与 simd 的相对关系。
#   - 若主机不支持 AVX2，两次运行都会落到 scalar，speedup ≈ 1.0x。
#   - 默认数据集列为 (id INT64, value DOUBLE)，value ~ U(0, 1)；脚本只做只读查询。
#   - SIMD 收益集中在谓词下推的 compare kernel，选择率越低越明显；投影/结果收集
#     （单线程）与进程启动开销会稀释端到端加速比。
#     实测参考（4M 行，single 模式）：DOUBLE 谓词约 1.2x~1.3x；INT64 谓词接近 1.0x。

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
MODE="single"

DO_BUILD=1
DO_GEN=1
BIN_OVERRIDE=""
declare -a QUERIES=()

usage() {
    cat <<'EOF'
在同一个二进制上对比 SIMD(AVX2) 与 非 SIMD(scalar) 后端的整条 SQL 执行耗时。

用法：
  ./benchmark/bench_simd.sh [options]

选项：
  --rows N       数据集行数（默认 4000000）
  --batch B      写入侧 DataChunk 行数（默认 8192）
  --queue Q      并行批队列容量（默认 16）
  --threads N    worker 数（默认 = nproc；仅 --mode multi 生效）
  --repeats R    每个后端的计时轮数（默认 5）
  --warmup W     每个后端计时前的预热次数（默认 2）
  --table NAME   生成的数据集表名（默认 bench_data）
  --db DIR       数据库根目录（默认 <repo>/benchmark/bench_data/modes_db）
  --sql "..."    追加一条待测 SQL（可多次；必须以 ';' 结尾）
  --mode M       执行模式 single|multi（额外参数，默认 single）
  --no-gen       跳过数据集生成，直接使用 --db 中已存在的表
  --bin PATH     指定 simple_olap 二进制路径
  --no-build     跳过 cmake 构建，直接使用已有二进制
  -h, --help     显示帮助

说明：
  SIMD 后端通过 SIMPLE_OLAP_FORCE_SCALAR=1 环境变量强制关闭，从而在同一个
  二进制上对比两条路径；两个后端的返回行数会逐条校验是否一致。
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
        --mode)    MODE="$2"; shift 2 ;;
        --no-gen)  DO_GEN=0; shift ;;
        --bin)     BIN_OVERRIDE="$2"; shift 2 ;;
        --no-build) DO_BUILD=0; shift ;;
        -h|--help) usage ;;
        *) echo "unknown argument: $1" >&2; exit 1 ;;
    esac
done

if [[ "${MODE}" != "single" && "${MODE}" != "multi" ]]; then
    echo "--mode must be single|multi, got: ${MODE}" >&2
    exit 1
fi

# 半数行阈值：默认复合谓词 SQL 用 < HALF，保证选择率不随 --rows 极端变化。
HALF=$(( ROWS / 2 ))

# 默认查询负载：单选率扫描 / 过滤后聚合 / 复合谓词（两个 SelectionMask AND）
if [[ ${#QUERIES[@]} -eq 0 ]]; then
    QUERIES+=(
        "SELECT id, value FROM ${TABLE} WHERE value > 0.5;"
        "SELECT COUNT(*) FROM ${TABLE} WHERE value > 0.5;"
        "SELECT COUNT(*) FROM ${TABLE} WHERE value > 0.5 AND id < ${HALF};"
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

# 传给每次运行的公共参数：数据库目录 / worker 数 / 队列容量 / 执行模式 / 静默
RUN_FLAGS=(--db "${DB_DIR}" --threads "${THREADS}" --queue "${QUEUE}" --mode "${MODE}" --silent)

ns_now() { date +%s%N; }

# 按后端运行一条 SQL；scalar 通过 SIMPLE_OLAP_FORCE_SCALAR 强制关闭 AVX2。
run_sql() {
    local backend="$1" sql="$2"
    if [[ "${backend}" == "scalar" ]]; then
        printf '%s\nexit\n' "${sql}" \
            | env SIMPLE_OLAP_FORCE_SCALAR=1 "${BIN}" "${RUN_FLAGS[@]}"
    else
        printf '%s\nexit\n' "${sql}" \
            | "${BIN}" "${RUN_FLAGS[@]}"
    fi
}

# 执行一条 SQL，返回本次运行的纳秒耗时
time_once() {
    local backend="$1" sql="$2" start end
    start="$(ns_now)"
    run_sql "${backend}" "${sql}" >/dev/null 2>&1 || true
    end="$(ns_now)"
    echo "$(( end - start ))"
}

# 仅启动、不执行任何查询的纳秒耗时（进程 + 打开 database 的固定开销）
time_startup_once() {
    local backend="$1" start end
    start="$(ns_now)"
    run_sql "${backend}" "" >/dev/null 2>&1 || true
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
    local backend="$1" sql="$2" out
    out="$(run_sql "${backend}" "${sql}" 2>/dev/null \
        | sed -n 's/.*(\([0-9][0-9]*\) rows).*/\1/p' | tail -n 1)"
    echo "${out:-0}"
}

# 主机是否支持 AVX2（决定 simd 与 scalar 是否真的走不同路径）
if grep -qm1 -E '(^| )avx2( |$)' /proc/cpuinfo 2>/dev/null; then
    HOST_AVX2=1
else
    HOST_AVX2=0
fi

printf '\n=== SIMD(AVX2) vs 非 SIMD(scalar)：整条 SQL 执行耗时对比 ===\n'
printf 'binary    : %s\n' "${BIN}"
printf 'db        : %s\n' "${DB_DIR}"
if [[ "${DO_GEN}" -eq 1 ]]; then
    printf 'dataset   : %s (generated: rows=%s batch=%s)\n' "${TABLE}" "${ROWS}" "${BATCH}"
else
    printf 'dataset   : %s (reused, --no-gen)\n' "${TABLE}"
fi
printf 'mode      : %s (threads=%s queue=%s)\n' "${MODE}" "${THREADS}" "${QUEUE}"
printf 'repeats   : %s (+%s warmup)\n' "${REPEATS}" "${WARMUP}"
if [[ "${HOST_AVX2}" -eq 1 ]]; then
    printf 'cpu       : AVX2 available -> simd=AVX2, scalar=forced-scalar\n'
else
    printf 'cpu       : no AVX2 detected -> both runs use scalar (speedup ~1.0x)\n'
fi

# 启动基线：两个后端共享同一份固定开销，分别测量以确认 override 生效前后一致
for backend in scalar simd; do
    samples=""
    for (( w = 0; w < WARMUP; ++w )); do time_startup_once "${backend}" >/dev/null; done
    for (( r = 0; r < REPEATS; ++r )); do
        samples+="$(time_startup_once "${backend}")"$'\n'
    done
    read -r _su_min su_med _su_avg <<<"$(printf '%s' "${samples}" | compute_stats)"
    printf 'startup   : %-6s median %s ms\n' "${backend}" "$(ms "${su_med}")"
done
echo

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

    row_scalar="$(rows_of scalar "${sql}")"
    row_simd="$(rows_of simd "${sql}")"
    if [[ "${row_scalar}" == "${row_simd}" ]]; then
        printf '    rows    : scalar=%s simd=%s  [OK]\n' "${row_scalar}" "${row_simd}"
    else
        printf '    rows    : scalar=%s simd=%s  [MISMATCH]\n' "${row_scalar}" "${row_simd}"
    fi

    declare -A med_of=()
    for backend in scalar simd; do
        for (( w = 0; w < WARMUP; ++w )); do time_once "${backend}" "${sql}" >/dev/null; done

        samples=""
        for (( r = 0; r < REPEATS; ++r )); do
            samples+="$(time_once "${backend}" "${sql}")"$'\n'
        done

        read -r mn md avg <<<"$(printf '%s' "${samples}" | compute_stats)"
        med_of["${backend}"]="${md}"

        printf '    %-6s  : min %s ms | median %s ms | mean %s ms\n' \
            "${backend}" "$(ms "${mn}")" "$(ms "${md}")" "$(ms "${avg}")"
    done

    sp="$(awk -v s="${med_of[scalar]}" -v m="${med_of[simd]}" \
        'BEGIN { if (m > 0) printf "%.2f", s / m; else printf "0.00" }')"
    printf '    speedup : %sx (median scalar / median simd)\n\n' "${sp}"

    SUMMARY+=("$(printf '%-*s %11s %11s %10s' "${WIDTH}" "${sql}" \
        "$(ms "${med_of[scalar]}")" "$(ms "${med_of[simd]}")" "${sp}x")")
done

echo "=== Summary (median, ms) ==="
printf '%-*s %11s %11s %10s\n' "${WIDTH}" "SQL" "scalar" "simd" "speedup"
printf '%-*s %11s %11s %10s\n' "${WIDTH}" \
    "$(printf '%*s' "${WIDTH}" '' | tr ' ' '-')" "-----------" "-----------" "----------"
for line in "${SUMMARY[@]}"; do
    echo "${line}"
done
echo
echo "提示：SIMD 收益集中在 storage 谓词下推的 compare kernel（选择率越低越明显）；"
echo "      投影/结果收集（单线程）与进程启动开销会稀释端到端加速比。"
echo "      可调参数：--rows / --batch / --queue / --threads / --mode / --repeats / --warmup。"
