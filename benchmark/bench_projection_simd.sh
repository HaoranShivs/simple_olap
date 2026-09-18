#!/usr/bin/env bash
#
# 验证并度量「ProjectionOperator 的 SIMD/整列求值路径」。
#
# 背景：
#   上一阶段的 SIMD 覆盖了 storage 谓词下推与 execution 层 Filter 谓词。
#   本阶段把同一套 KernelRegistry 接到 ProjectionOperator：
#   Init() 时把 SELECT 列表里的表达式一次性编译成 VectorExpression
#   （同类型列 +/- 同类型列、列 +/- 可精确收敛的字面量），
#   Next() 在 dense 输入（sel_vector 为空）下直接对整列调用算术内核，
#   不再逐行 ExecExpression::Eval() + variant 构造 + WriteExecValue。
#
#   分类规则（Init 阶段）：
#     - 纯列引用        -> DIRECT_COLUMN（沿用既有零拷贝 view 投影）
#     - 可编译算术表达式 -> VECTOR_EXPRESSION（dense 走内核）
#     - 其余            -> SCALAR_EXPRESSION（逐行物化，语义不变）
#
#   回落规则：
#     - selection 非空（带 WHERE）时不做 gather，整批退回逐行物化；
#     - 类型无法精确收敛（如 value + id：DOUBLE 列 + INT64 列）也不编译。
#
# 脚本内容：
#   [校验] 同一份数据、同一批 SQL，分别用 AVX2 后端与强制 scalar 后端
#          （SIMPLE_OLAP_FORCE_SCALAR=1）执行，diff 完整 stdout。
#          两者必须完全一致（两者共用同一套 typed 语义内核，含尾部）。
#          single 模式逐字节严格比较；multi 模式并行扫描的 batch 到达顺序
#          不确定，行序不保证，故按行排序后比较（值多重集合必须一致）。
#   [计时] 对若干 Projection 负载在两种后端下取 median，输出 speedup。
#
# 用法：
#   ./benchmark/bench_projection_simd.sh
#   ./benchmark/bench_projection_simd.sh --rows 2000000 --repeats 7 --warmup 3
#   ./benchmark/bench_projection_simd.sh --mode multi --threads 8
#   ./benchmark/bench_projection_simd.sh --no-gen --db <dir> --table bench_data
#   ./benchmark/bench_projection_simd.sh --no-build
#
# 参数（默认值与其它 benchmark 脚本对齐）：
#   --rows N       数据集行数（默认 2000000）
#   --batch B      写入侧 DataChunk 行数（默认 8192）
#   --queue Q      并行批队列容量（默认 16）
#   --threads N    worker 数（默认 = nproc；仅 --mode multi 生效）
#   --repeats R    每个后端的计时轮数（默认 5）
#   --warmup W     每个后端计时前的预热次数（默认 2）
#   --table NAME   生成的数据集表名（默认 bench_data）
#   --db DIR       数据库根目录（默认 <repo>/benchmark/bench_data/projection_db）
#   --mode M       执行模式 single|multi（默认 single）
#   --no-gen       跳过数据集生成，复用 --db 中已有表
#   --bin PATH     指定 simple_olap 二进制路径
#   --no-build     跳过 cmake 构建
#   -h, --help     显示帮助

set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${ROOT_DIR}/build"
BUILD_TYPE="${BUILD_TYPE:-Release}"
JOBS="$(nproc 2>/dev/null || echo 4)"

ROWS=2000000
BATCH=8192
QUEUE=16
TABLE="bench_data"
THREADS="$(nproc 2>/dev/null || echo 4)"
REPEATS=5
WARMUP=2
DB_DIR="${ROOT_DIR}/benchmark/bench_data/projection_db"
MODE="single"

DO_BUILD=1
DO_GEN=1
BIN_OVERRIDE=""

usage() {
    cat <<'EOF'
验证并度量 ProjectionOperator 的 SIMD/整列求值路径。

用法：
  ./benchmark/bench_projection_simd.sh [options]

选项：
  --rows N       数据集行数（默认 2000000）
  --batch B      写入侧 DataChunk 行数（默认 8192）
  --queue Q      并行批队列容量（默认 16）
  --threads N    worker 数（默认 = nproc；仅 --mode multi 生效）
  --repeats R    每个后端的计时轮数（默认 5）
  --warmup W     每个后端的计时前的预热次数（默认 2）
  --table NAME   生成的数据集表名（默认 bench_data）
  --db DIR       数据库根目录（默认 <repo>/benchmark/bench_data/projection_db）
  --mode M       执行模式 single|multi（默认 single）
  --no-gen       跳过数据集生成，复用 --db 中已有表
  --bin PATH     指定 simple_olap 二进制路径
  --no-build     跳过 cmake 构建
  -h, --help     显示帮助

说明：
  [校验] 用 SIMPLE_OLAP_FORCE_SCALAR=1 在同一个二进制上关掉 AVX2，
         对同一批 Projection SQL 做完整 stdout diff；不一致即失败。
         single 模式逐字节比较；multi 模式先按行排序再去重比较行序。
  [计时] 负载为「会落到 Projection 的 dense 整列算术表达式」，以及
         一个带 WHERE(selection 非空) 的回落形态。
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

THIRD=$(( ROWS / 3 ))
HIGH=$(( ROWS - 2000 ))

# 正确性校验 SQL。
#   前两条是整表 dense 输出的整列算术（真正走 VECTOR_EXPRESSION / AVX2），
#   虽然输出很大，但这是唯一能逐字节验证 dense SIMD 路径的方式（没有 LIMIT）。
#   其余为选择性谓词，输出很小，覆盖：selection 非空回落、非可编译表达式回落、
#   全直接列引用、混合 slot 形态。
build_queries() {
    cat <<EOF
SELECT value + 1.0 FROM ${TABLE};
SELECT value + value FROM ${TABLE};
SELECT id + 1 FROM ${TABLE} WHERE id <= 2000;
SELECT 1.0 - value FROM ${TABLE} WHERE id <= 2000;
SELECT value - 0.25 FROM ${TABLE} WHERE id <= 2000;
SELECT value + 1.0, id FROM ${TABLE} WHERE id <= 2000;
SELECT value + 1.0 FROM ${TABLE} WHERE value > 0.9;
SELECT value + id FROM ${TABLE} WHERE value > 0.9995;
SELECT id, value FROM ${TABLE} WHERE value > 0.9995;
SELECT value + 1.0, value - 1.0, id FROM ${TABLE} WHERE id > ${HIGH};
exit
EOF
}

# 计时负载（dense 整列算术 + 一个 selection 回落形态）
build_perf_queries() {
    cat <<EOF
SELECT value + 1.0 FROM ${TABLE};
SELECT value - 0.25 FROM ${TABLE};
SELECT value + value FROM ${TABLE};
SELECT 1.0 - value FROM ${TABLE};
SELECT value + 1.0, value - 1.0 FROM ${TABLE};
SELECT id + 1 FROM ${TABLE};
SELECT value + 1.0, id FROM ${TABLE};
SELECT value + 1.0 FROM ${TABLE} WHERE id > ${THIRD};
EOF
}

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

if [[ "${DO_GEN}" -eq 1 ]]; then
    echo "[2/3] generate dataset: rows=${ROWS} batch=${BATCH} -> ${DB_DIR}"
    rm -rf "${DB_DIR}"
    mkdir -p "$(dirname "${DB_DIR}")"
    "${BIN}" --db "${DB_DIR}" --gen-table "${TABLE}" --rows "${ROWS}" --batch "${BATCH}"
else
    echo "[2/3] skip dataset generation (--no-gen), using ${DB_DIR}"
fi

RUN_FLAGS=(--db "${DB_DIR}" --threads "${THREADS}" --queue "${QUEUE}" --mode "${MODE}")

ns_now() { date +%s%N; }

run_sql() {
    local backend="$1" silent="$2" sql="$3"
    local -a flags=("${RUN_FLAGS[@]}")
    [[ "${silent}" == "1" ]] && flags+=(--silent)
    if [[ "${backend}" == "scalar" ]]; then
        printf '%s\n' "${sql}" | env SIMPLE_OLAP_FORCE_SCALAR=1 "${BIN}" "${flags[@]}"
    else
        printf '%s\n' "${sql}" | "${BIN}" "${flags[@]}"
    fi
}

time_once() {
    local backend="$1" sql="$2" start end
    start="$(ns_now)"
    run_sql "${backend}" 1 "${sql}" >/dev/null 2>&1 || true
    end="$(ns_now)"
    echo "$(( end - start ))"
}

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

if grep -qm1 -E '(^| )avx2( |$)' /proc/cpuinfo 2>/dev/null; then
    HOST_AVX2=1
else
    HOST_AVX2=0
fi

printf '\n=== ProjectionOperator SIMD：正确性校验 + 性能对比 ===\n'
printf 'binary : %s\n' "${BIN}"
printf 'db     : %s\n' "${DB_DIR}"
printf 'data   : %s (rows=%s batch=%s)\n' "${TABLE}" "${ROWS}" "${BATCH}"
printf 'mode   : %s (threads=%s queue=%s)\n' "${MODE}" "${THREADS}" "${QUEUE}"
if [[ "${HOST_AVX2}" -eq 1 ]]; then
    printf 'cpu    : AVX2 available -> simd=AVX2, scalar=forced-scalar\n'
else
    printf 'cpu    : no AVX2 detected -> both runs use scalar (speedup ~1.0x)\n'
fi

# ---------- 正确性校验 ----------
printf '\n[3/3] correctness: diff full stdout (scalar vs simd) ...\n'
QUERY_FILE="$(mktemp)"
OUT_SIMD="$(mktemp)"
OUT_SCALAR="$(mktemp)"
trap 'rm -f "${QUERY_FILE}" "${OUT_SIMD}" "${OUT_SCALAR}" "${OUT_SIMD}.sorted" "${OUT_SCALAR}.sorted"' EXIT

build_queries > "${QUERY_FILE}"

run_sql simd   0 "$(cat "${QUERY_FILE}")" > "${OUT_SIMD}"   2>&1 || true
run_sql scalar 0 "$(cat "${QUERY_FILE}")" > "${OUT_SCALAR}" 2>&1 || true

# multi 模式下并行扫描的 batch 到达顺序不确定，行序天然不保证稳定；
# 因此 multi 模式按行排序后比较（值的多重集合必须完全一致），
# single 模式仍做逐字节严格比较。
CMP_SCALAR="${OUT_SCALAR}"
CMP_SIMD="${OUT_SIMD}"
SORTED_NOTE=""
if [[ "${MODE}" == "multi" ]]; then
    sort "${OUT_SCALAR}" > "${OUT_SCALAR}.sorted"
    sort "${OUT_SIMD}"    > "${OUT_SIMD}.sorted"
    CMP_SCALAR="${OUT_SCALAR}.sorted"
    CMP_SIMD="${OUT_SIMD}.sorted"
    SORTED_NOTE=" (sorted; multi 模式行序不确定)"
fi

if diff -u "${CMP_SCALAR}" "${CMP_SIMD}" > /dev/null; then
    n_queries="$(grep -c ';' "${QUERY_FILE}")"
    printf '    %s queries: scalar == simd%s  [OK]\n' "${n_queries}" "${SORTED_NOTE}"
    CHECK_OK=1
else
    printf '    scalar != simd  [MISMATCH]\n'
    diff -u "${CMP_SCALAR}" "${CMP_SIMD}" | head -60
    CHECK_OK=0
fi

# ---------- 性能对比 ----------
printf '\n--- performance: Projection SQL, median of %s (+%s warmup) ---\n' "${REPEATS}" "${WARMUP}"

declare -a SUMMARY=()
while IFS= read -r sql; do
    [[ -z "${sql}" ]] && continue
    [[ "${sql}" == "exit" ]] && continue

    declare -A med_of=()
    for backend in scalar simd; do
        for (( w = 0; w < WARMUP; ++w )); do time_once "${backend}" "${sql}" >/dev/null; done
        samples=""
        for (( r = 0; r < REPEATS; ++r )); do
            samples+="$(time_once "${backend}" "${sql}")"$'\n'
        done
        read -r mn md avg <<<"$(printf '%s' "${samples}" | compute_stats)"
        med_of["${backend}"]="${md}"
        printf '  %s\n    %-6s: min %s ms | median %s ms | mean %s ms\n' \
            "${sql}" "${backend}" "$(ms "${mn}")" "$(ms "${md}")" "$(ms "${avg}")"
    done

    sp="$(awk -v s="${med_of[scalar]}" -v m="${med_of[simd]}" \
        'BEGIN { if (m > 0) printf "%.2f", s / m; else printf "0.00" }')"
    printf '    speedup: %sx (median scalar / median simd)\n' "${sp}"
    SUMMARY+=("$(printf '%s|%s|%s|%s' "${sql}" "$(ms "${med_of[scalar]}")" "$(ms "${med_of[simd]}")" "${sp}")")
done < <(build_perf_queries)

WIDTH=3
for row in "${SUMMARY[@]}"; do
    q="${row%%|*}"
    (( ${#q} > WIDTH )) && WIDTH=${#q}
done

printf '\n=== Summary (median, ms) ===\n'
printf '%-*s %11s %11s %10s\n' "${WIDTH}" "SQL" "scalar" "simd" "speedup"
printf '%-*s %11s %11s %10s\n' "${WIDTH}" \
    "$(printf '%*s' "${WIDTH}" '' | tr ' ' '-')" "-----------" "-----------" "----------"
for row in "${SUMMARY[@]}"; do
    IFS='|' read -r q s m sp <<<"${row}"
    printf '%-*s %11s %11s %10s\n' "${WIDTH}" "${q}" "${s}" "${m}" "${sp}x"
done

if [[ "${CHECK_OK}" -eq 1 ]]; then
    printf '\ncorrectness: [OK]\n'
    exit 0
fi
printf '\ncorrectness: [MISMATCH]\n'
exit 1
