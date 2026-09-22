#!/usr/bin/env bash
# Compare.md 三方对比运行脚本：
#   Simple Queue (b096352) / Simple Direct (8d0906f) / DuckDB (v1.5.5)
#
# 前置：
#   1. 三个引擎的 benchmark 可执行存放在 $BIN_DIR
#      bench_simple_queue / bench_simple_direct / bench_duckdb
#   2. medium 数据集已生成：
#      bench_simple_* --db <dir> --generate --rows 4194304
#      bench_duckdb   --db <file> --generate --rows 4194304
#
# 用法：
#   BIN_DIR=/tmp/qps_compare DATA_DIR=/tmp/qps_compare \
#   SIMPLE_QUEUE_DB=... SIMPLE_DIRECT_DB=... DUCKDB_DB=... \
#   THREADS="1 2 4 8" WARMUP=2 MEASURE=5 \
#   ./benchmark/qps/run_compare.sh
set -u

BIN_DIR=${BIN_DIR:-/tmp/qps_compare}
DATA_DIR=${DATA_DIR:-/tmp/qps_compare}
CSV=${CSV:-${DATA_DIR}/compare_results.csv}
THREADS=${THREADS:-"1 2 4 8"}
WARMUP=${WARMUP:-2}
MEASURE=${MEASURE:-5}
ROWS=${ROWS:-4194304}
DATASET=${DATASET:-medium}
QUERY=${QUERY:-all}

SIMPLE_QUEUE_DB=${SIMPLE_QUEUE_DB:-${DATA_DIR}/simple_queue_${DATASET}}
SIMPLE_DIRECT_DB=${SIMPLE_DIRECT_DB:-${DATA_DIR}/simple_direct_${DATASET}}
DUCKDB_DB=${DUCKDB_DB:-${DATA_DIR}/duckdb_${DATASET}.duckdb}

SIMPLE_QUEUE_HASH=${SIMPLE_QUEUE_HASH:-b096352}
SIMPLE_DIRECT_HASH=${SIMPLE_DIRECT_HASH:-8d0906f}
DUCKDB_HASH=${DUCKDB_HASH:-d8cdaa33}

rm -f "${CSV}"

cpu_set_for_threads() {
    case "$1" in
        1|2|4) echo "0-3" ;;  # 4 物理核（SMT 兄弟为 4-7）
        *)     echo "0-7" ;;
    esac
}

log() {
    printf '\n[%s] %s\n' "$(date +%H:%M:%S)" "$*"
}

run_engine() {
    local engine=$1 binary=$2 db=$3 hash=$4 threads=$5
    local cpu_set
    cpu_set=$(cpu_set_for_threads "${threads}")

    log "run ${engine} threads=${threads} cpus=${cpu_set}"
    taskset -c "${cpu_set}" "${binary}" \
        --db "${db}" \
        --engine "${engine}" \
        --commit "${hash}" \
        --dataset "${DATASET}" \
        --rows "${ROWS}" \
        --query "${QUERY}" \
        --threads "${threads}" \
        --warmup "${WARMUP}" \
        --measure "${MEASURE}" \
        --csv "${CSV}"
}

for t in ${THREADS}; do
    # 轮换引擎顺序，避免固定顺序带来的温度 / 后台噪声偏差
    case $((t % 3)) in
        0) order="simple_queue duckdb simple_direct" ;;
        1) order="simple_direct simple_queue duckdb" ;;
        2) order="duckdb simple_direct simple_queue" ;;
    esac

    for engine in ${order}; do
        case "${engine}" in
            simple_queue)  run_engine "simple_queue"  "${BIN_DIR}/bench_simple_queue"  "${SIMPLE_QUEUE_DB}"  "${SIMPLE_QUEUE_HASH}"  "${t}" ;;
            simple_direct) run_engine "simple_direct" "${BIN_DIR}/bench_simple_direct" "${SIMPLE_DIRECT_DB}" "${SIMPLE_DIRECT_HASH}" "${t}" ;;
            duckdb)        run_engine "duckdb"        "${BIN_DIR}/bench_duckdb"        "${DUCKDB_DB}"        "${DUCKDB_HASH}"        "${t}" ;;
        esac
    done
done

log "done: ${CSV}"
