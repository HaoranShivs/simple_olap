#!/usr/bin/env bash
#
# verify_simd.sh —— scalar / AVX2 / single / multi 正确性校验（只做正确性，不做计时）
#
# BenchmarkSettingV2 的核心整改之一：
#   - shell 不再负责纳秒级计时（计时全部交给 benchmark 可执行文件）；
#   - correctness 不再只比较 row count，而是比较与行序无关的 ResultDigest
#     （row_count + hash1 + hash2）；
#   - multi 模式输出顺序与 single 不同也不再产生 false mismatch。
#
# 做法：在同一个 simple_olap 二进制上用 --digest 跑同一批 SQL：
#
#     (scalar, single) vs (avx2, single) vs (avx2, multi)
#
# 三者的摘要必须完全一致；任何查询失败都立即中止（不吞错误）。
#
# 用法：
#   ./benchmark/verify_simd.sh
#   ./benchmark/verify_simd.sh --rows 1000000 --db benchmark/bench_data/verify_db
#   ./benchmark/verify_simd.sh --no-build --no-gen
#   ./benchmark/verify_simd.sh --sql "SELECT ...;"

set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${ROOT_DIR}/build"
BUILD_TYPE="${BUILD_TYPE:-Release}"
JOBS="$(nproc 2>/dev/null || echo 4)"

ROWS=1000000
BATCH=8192
TABLE="bench_data"
THREADS="$(nproc 2>/dev/null || echo 4)"
QUEUE=16
DB_DIR="${ROOT_DIR}/benchmark/bench_data/verify_db"

DO_BUILD=1
DO_GEN=1
BIN_OVERRIDE=""
declare -a QUERIES=()

usage() {
    cat <<'EOF'
在同一个二进制上校验 scalar / AVX2、single / multi 的结果摘要一致。

用法：
  ./benchmark/verify_simd.sh [options]

选项：
  --rows N       数据集行数（默认 1000000）
  --batch B      写入侧 DataChunk 行数（默认 8192，仅数据生成）
  --queue Q      并行批队列容量（默认 16）
  --threads N    multi 模式 worker 数（默认 = nproc）
  --table NAME   数据集表名（默认 bench_data）
  --db DIR       数据库根目录（默认 <repo>/benchmark/bench_data/verify_db）
  --sql "..."    追加待校验 SQL（可多次；必须以 ';' 结尾）
  --no-gen       跳过数据集生成
  --bin PATH     指定 simple_olap 二进制
  --no-build     跳过 cmake 构建
  -h, --help     显示帮助
EOF
    exit 0
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --rows)    ROWS="$2"; shift 2 ;;
        --batch)   BATCH="$2"; shift 2 ;;
        --queue)   QUEUE="$2"; shift 2 ;;
        --threads) THREADS="$2"; shift 2 ;;
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

if [[ ${#QUERIES[@]} -eq 0 ]]; then
    HALF=$(( ROWS / 2 ))
    QUERIES+=(
        "SELECT id, value FROM ${TABLE} WHERE value > 0.5;"
        "SELECT COUNT(*) FROM ${TABLE} WHERE value > 0.5;"
        "SELECT COUNT(*) FROM ${TABLE} WHERE value > 0.5 AND id < ${HALF};"
        "SELECT COUNT(*) FROM ${TABLE} WHERE id > ${HALF};"
        "SELECT id, value + 1.0 FROM ${TABLE} WHERE value > 0.99;"
    )
fi

if [[ "${DO_BUILD}" -eq 1 ]]; then
    echo "[1/3] configure + build (${BUILD_TYPE}) ..."
    cmake -S "${ROOT_DIR}" -B "${BUILD_DIR}" -DCMAKE_BUILD_TYPE="${BUILD_TYPE}" >/dev/null
    cmake --build "${BUILD_DIR}" -j"${JOBS}" --target simple_olap >/dev/null
fi

BIN="${BIN_OVERRIDE:-${BUILD_DIR}/bin/simple_olap}"
if [[ ! -x "${BIN}" ]]; then
    echo "binary not found: ${BIN}" >&2
    exit 1
fi

if [[ "${DO_GEN}" -eq 1 ]]; then
    echo "[2/3] generate dataset: rows=${ROWS} -> ${DB_DIR}"
    rm -rf "${DB_DIR}"
    "${BIN}" --db "${DB_DIR}" --gen-table "${TABLE}" --rows "${ROWS}" --load-batch "${BATCH}"
else
    echo "[2/3] skip dataset generation (--no-gen), using ${DB_DIR}"
fi

RUN_FLAGS=(--db "${DB_DIR}" --threads "${THREADS}" --queue "${QUEUE}" --digest)

# 执行一条 SQL，输出 "rows=.. hash1=.. hash2=.."；失败立即 abort（不吞错误）。
digest_of() {
    local backend="$1" mode="$2" sql="$3"
    local out
    if [[ "${backend}" == "scalar" ]]; then
        out="$(printf '%s\nexit\n' "${sql}" | env SIMPLE_OLAP_FORCE_SCALAR=1 "${BIN}" \
            "${RUN_FLAGS[@]}" --mode "${mode}" 2>&1)"
    else
        out="$(printf '%s\nexit\n' "${sql}" | "${BIN}" "${RUN_FLAGS[@]}" --mode "${mode}" 2>&1)"
    fi
    local digest
    digest="$(printf '%s\n' "${out}" | sed -n 's/.*digest //p' | tail -n 1)"
    if [[ -z "${digest}" ]]; then
        echo "query failed (${backend}/${mode}): ${sql}" >&2
        echo "${out}" >&2
        exit 1
    fi
    echo "${digest}"
}

echo "[3/3] digest comparison (scalar/single vs avx2/single vs avx2/multi)"
printf 'binary : %s\n' "${BIN}"
printf 'db     : %s\n\n' "${DB_DIR}"

fail=0
idx=0
for sql in "${QUERIES[@]}"; do
    idx=$(( idx + 1 ))
    d_scalar="$(digest_of scalar single "${sql}")"
    d_avx2="$(digest_of avx2 single "${sql}")"
    d_multi="$(digest_of avx2 multi "${sql}")"

    if [[ "${d_scalar}" == "${d_avx2}" && "${d_avx2}" == "${d_multi}" ]]; then
        printf '[%d] OK   %s\n      %s\n' "${idx}" "${sql}" "${d_scalar}"
    else
        printf '[%d] FAIL %s\n' "${idx}" "${sql}"
        printf '      scalar/single: %s\n      avx2/single  : %s\n      avx2/multi   : %s\n' \
               "${d_scalar}" "${d_avx2}" "${d_multi}"
        fail=1
    fi
done

if [[ "${fail}" -ne 0 ]]; then
    echo "verify_simd: FAILED" >&2
    exit 1
fi
echo "verify_simd: all digests agree [OK]"
