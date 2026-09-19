#!/usr/bin/env bash
#
# run_perf_suite.sh —— simple_olap 统一性能测试流程
#
# 只调用同一个二进制 bench_query_workload（真实 Connection 生命周期），
# AVX2 A/B 与内存消融全部依靠「同一二进制 + 独立进程 + 环境变量 / 参数」：
#
#   scalar : SIMPLE_OLAP_FORCE_SCALAR=1
#   M0     : --memory system --buffer-mode direct --block-cache 0  --buffer-cache 0
#   M1     : --memory arena  --buffer-mode direct --block-cache 0  --buffer-cache 0
#   M2     : --memory arena  --buffer-mode pooled --block-cache 64 --buffer-cache 0
#   M3     : --memory arena  --buffer-mode pooled --block-cache 64 --buffer-cache 64
#
# 所有配置都在 CSV 中按 result_mode（engine / materialized）分开记录，
# `--result-mode both` 一次进程同时产出两行。
#
# 产出：
#   benchmark/results/perf_suite_<ts>.csv      所有端到端测量（一行一个 result_mode）
#   benchmark/results/summary_<ts>.txt        按配置聚合的 median QPS/P50/P99
#   benchmark/results/micro_<ts>.txt          五个微基准原始输出
#
# 用法：
#   ./benchmark/run_perf_suite.sh                     # 正式（耗时较长）
#   ./benchmark/run_perf_suite.sh --quick             # 快速自检（短时长 / 小数据）
#   ./benchmark/run_perf_suite.sh --no-build --no-gen # 复用已有构建与数据
#   ./benchmark/run_perf_suite.sh --skip-memory       # 跳过某个阶段
#
# 选项：
#   --quick              等价于 --rows 200000 --latency-rows 200000 --warmup 1
#                        --duration 2 --repeats 1 --clients "1 4" --min-samples 0
#   --rows N             标准数据集行数（默认 4000000）
#   --latency-rows N     P99/内存消融数据集行数（默认 1000000；quick 下同 --rows）
#   --warmup S           预热秒数（默认 5）
#   --duration S         每轮测量秒数（默认 30）
#   --repeats R          每个配置的重复轮数（默认 5，取 median，A/B 顺序交错）
#   --clients "1 2 4 8"  并发 client 列表
#   --min-samples N      P99 最少样本数（默认 5000；不足会自动延长测量）
#   --result-mode MODE   engine|prepared|materialized|both|all（默认 both）
#   --cpus "0-3"         将每次 benchmark 运行绑定到指定 CPU 集合（taskset，可选）
#   --db DIR             标准数据集数据库目录
#   --latency-db DIR     内存消融数据集数据库目录
#   --csv PATH           指定输出 CSV
#   --no-build           跳过 cmake 构建
#   --no-gen             跳过数据集生成
#   --skip-smoke / --skip-qps / --skip-parallel / --skip-avx2 / --skip-memory / --skip-micro
#   -h, --help           显示帮助

set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${ROOT_DIR}/build"
BUILD_TYPE="${BUILD_TYPE:-Release}"
JOBS="$(nproc 2>/dev/null || echo 4)"

ROWS=4000000
LATENCY_ROWS=1000000
WARMUP=5
DURATION=30
REPEATS=5
MIN_SAMPLES=5000
CLIENTS_LIST="1 2 4 8"
RESULT_MODE="both"
TASKSET_CPUS=""
STD_DB="${ROOT_DIR}/benchmark/bench_data/perf_db"
LATENCY_DB="${ROOT_DIR}/benchmark/bench_data/perf_db_latency"
RESULTS_DIR="${ROOT_DIR}/benchmark/results"
TIMESTAMP="$(date +%Y%m%d_%H%M%S)"
CSV=""

DO_BUILD=1
DO_GEN=1
DO_SMOKE=1
DO_QPS=1
DO_PARALLEL=1
DO_AVX2=1
DO_MEMORY=1
DO_MICRO=1

usage() {
    sed -n '2,45p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'
    exit 0
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --quick)
            ROWS=200000; LATENCY_ROWS=200000; WARMUP=1; DURATION=2; REPEATS=1
            CLIENTS_LIST="1 4"; MIN_SAMPLES=0; shift ;;
        --rows)          ROWS="$2"; shift 2 ;;
        --latency-rows)  LATENCY_ROWS="$2"; shift 2 ;;
        --warmup)        WARMUP="$2"; shift 2 ;;
        --duration)      DURATION="$2"; shift 2 ;;
        --repeats)       REPEATS="$2"; shift 2 ;;
        --clients)       CLIENTS_LIST="$2"; shift 2 ;;
        --min-samples)   MIN_SAMPLES="$2"; shift 2 ;;
        --result-mode)   RESULT_MODE="$2"; shift 2 ;;
        --cpus)          TASKSET_CPUS="$2"; shift 2 ;;
        --db)            STD_DB="$2"; shift 2 ;;
        --latency-db)    LATENCY_DB="$2"; shift 2 ;;
        --csv)           CSV="$2"; shift 2 ;;
        --no-build)      DO_BUILD=0; shift ;;
        --no-gen)        DO_GEN=0; shift ;;
        --skip-smoke)    DO_SMOKE=0; shift ;;
        --skip-qps)      DO_QPS=0; shift ;;
        --skip-parallel) DO_PARALLEL=0; shift ;;
        --skip-avx2)     DO_AVX2=0; shift ;;
        --skip-memory)   DO_MEMORY=0; shift ;;
        --skip-micro)    DO_MICRO=0; shift ;;
        -h|--help)       usage ;;
        *) echo "unknown argument: $1" >&2; exit 1 ;;
    esac
done

BIN="${BUILD_DIR}/bin/bench_query_workload"
mkdir -p "${RESULTS_DIR}"
[[ -n "${CSV}" ]] || CSV="${RESULTS_DIR}/perf_suite_${TIMESTAMP}.csv"
SUMMARY="${RESULTS_DIR}/summary_${TIMESTAMP}.txt"
MICRO_LOG="${RESULTS_DIR}/micro_${TIMESTAMP}.txt"

log() { printf '\n=== %s ===\n' "$*"; }

# ----------------------------------------------------------
# [1/7] 构建
# ----------------------------------------------------------
if [[ "${DO_BUILD}" -eq 1 ]]; then
    log "[1/7] build (${BUILD_TYPE})"
    cmake -S "${ROOT_DIR}" -B "${BUILD_DIR}" -DCMAKE_BUILD_TYPE="${BUILD_TYPE}" >/dev/null
    cmake --build "${BUILD_DIR}" -j"${JOBS}" >/dev/null
else
    log "[1/7] build skipped (--no-build)"
fi

if [[ ! -x "${BIN}" ]]; then
    echo "benchmark binary not found: ${BIN}" >&2
    exit 1
fi

if [[ -n "${TASKSET_CPUS}" ]]; then
    if ! command -v taskset >/dev/null 2>&1; then
        echo "--cpus given but taskset not found" >&2
        exit 1
    fi
    echo "cpu affinity: taskset -c ${TASKSET_CPUS}"
fi

# ----------------------------------------------------------
# [2/7] 数据集（标准 + latency 两套 profile）
# ----------------------------------------------------------
if [[ "${DO_GEN}" -eq 1 ]]; then
    log "[2/7] prepare datasets"
    rm -rf "${STD_DB}"
    "${BIN}" --db "${STD_DB}" --prepare --rows "${ROWS}"
    if [[ "${LATENCY_ROWS}" != "${ROWS}" ]]; then
        rm -rf "${LATENCY_DB}"
        "${BIN}" --db "${LATENCY_DB}" --prepare --rows "${LATENCY_ROWS}"
    else
        LATENCY_DB="${STD_DB}"
    fi
else
    log "[2/7] dataset generation skipped (--no-gen)"
fi

# ----------------------------------------------------------
# [3/7] correctness smoke + SIMD/single-multi digest 校验
# ----------------------------------------------------------
if [[ "${DO_SMOKE}" -eq 1 ]]; then
    log "[3/7] correctness"
    smoke_fail=0
    for test_bin in simd_compare_test gather_kernel_test mask_pipeline_test global_aggregate_test; do
        if [[ -x "${BUILD_DIR}/bin/${test_bin}" ]]; then
            if "${BUILD_DIR}/bin/${test_bin}" >/dev/null 2>&1; then
                echo "  ${test_bin} [OK]"
            else
                echo "  ${test_bin} [FAIL]"
                smoke_fail=1
            fi
        fi
    done
    if [[ "${smoke_fail}" -ne 0 ]]; then
        echo "correctness smoke failed; aborting" >&2
        exit 1
    fi

    if [[ -x "${ROOT_DIR}/benchmark/verify_simd.sh" ]]; then
        if [[ -n "${TASKSET_CPUS}" ]]; then
            taskset -c "${TASKSET_CPUS}" "${ROOT_DIR}/benchmark/verify_simd.sh" --no-build --no-gen --db "${STD_DB}" \
                --table perf_data --rows "${ROWS}"
        else
            "${ROOT_DIR}/benchmark/verify_simd.sh" --no-build --no-gen --db "${STD_DB}" --table perf_data \
                --rows "${ROWS}"
        fi
    fi
fi

# ----------------------------------------------------------
# 统一的 bench 调用包装
# ----------------------------------------------------------
run_bench() {
    local backend="$1" db="$2" workload="$3" clients="$4" rows="$5" min_samples="$6"
    shift 6
    local -a cmd=("${BIN}" --db "${db}" --workload "${workload}" --clients "${clients}" --rows "${rows}"
                  --warmup "${WARMUP}" --duration "${DURATION}" --result-mode "${RESULT_MODE}" --csv "${CSV}")
    if [[ "${min_samples}" != "0" ]]; then
        cmd+=(--min-samples "${min_samples}")
    fi
    cmd+=("$@")

    if [[ "${backend}" == "scalar" ]]; then
        if [[ -n "${TASKSET_CPUS}" ]]; then
            SIMPLE_OLAP_FORCE_SCALAR=1 taskset -c "${TASKSET_CPUS}" "${cmd[@]}" >/dev/null
        else
            SIMPLE_OLAP_FORCE_SCALAR=1 "${cmd[@]}" >/dev/null
        fi
    else
        if [[ -n "${TASKSET_CPUS}" ]]; then
            taskset -c "${TASKSET_CPUS}" "${cmd[@]}" >/dev/null
        else
            "${cmd[@]}" >/dev/null
        fi
    fi
}

repeat_bench() {
    local backend="$1" db="$2" workload="$3" clients="$4" rows="$5" min_samples="$6"
    shift 6
    local r
    for (( r = 0; r < REPEATS; ++r )); do
        run_bench "${backend}" "${db}" "${workload}" "${clients}" "${rows}" "${min_samples}" "$@"
    done
}

# paired A/B：按轮次交替 scalar / avx2 顺序，消除固定顺序的 boost / 温度偏差。
repeat_bench_pair() {
    local db="$1" workload="$2" clients="$3" rows="$4" min_samples="$5"
    shift 5
    local r
    for (( r = 0; r < REPEATS; ++r )); do
        if (( r % 2 == 0 )); then
            run_bench scalar "${db}" "${workload}" "${clients}" "${rows}" "${min_samples}" "$@"
            run_bench avx2 "${db}" "${workload}" "${clients}" "${rows}" "${min_samples}" "$@"
        else
            run_bench avx2 "${db}" "${workload}" "${clients}" "${rows}" "${min_samples}" "$@"
            run_bench scalar "${db}" "${workload}" "${clients}" "${rows}" "${min_samples}" "$@"
        fi
    done
}

# ----------------------------------------------------------
# [4/7] QPS：client 间并发（固定 SINGLE_THREAD）
# ----------------------------------------------------------
if [[ "${DO_QPS}" -eq 1 ]]; then
    log "[4/7] QPS matrix (single-thread execution, inter-query concurrency, result-mode=${RESULT_MODE})"
    for workload in q1 q2 q4 mixed; do
        for clients in ${CLIENTS_LIST}; do
            echo "  workload=${workload} clients=${clients} repeats=${REPEATS}"
            repeat_bench avx2 "${STD_DB}" "${workload}" "${clients}" "${ROWS}" 0 --mode single
        done
    done
fi

# ----------------------------------------------------------
# [5/7] 单 Query 内部并行（clients=1, MULTI_THREAD）
# ----------------------------------------------------------
if [[ "${DO_PARALLEL}" -eq 1 ]]; then
    log "[5/7] parallel scaling (1 query, MULTI_THREAD)"
    for workload in q1 q4 q5; do
        for threads in 1 2 4 8; do
            echo "  workload=${workload} threads=${threads} repeats=${REPEATS}"
            repeat_bench avx2 "${STD_DB}" "${workload}" 1 "${ROWS}" 0 --mode multi --threads "${threads}"
        done
    done
fi

# ----------------------------------------------------------
# [6/7] AVX2 端到端（同一二进制 + 独立进程 + paired 顺序）
# ----------------------------------------------------------
if [[ "${DO_AVX2}" -eq 1 ]]; then
    log "[6/7] AVX2 end-to-end (scalar vs avx2, paired order)"
    for workload in q1 q2 q3 q4 q5 mixed; do
        echo "  workload=${workload} clients=1 repeats=${REPEATS}"
        repeat_bench_pair "${STD_DB}" "${workload}" 1 "${ROWS}" 0 --mode single
    done
    for workload in mixed; do
        echo "  workload=${workload} clients=4 repeats=${REPEATS}"
        repeat_bench_pair "${STD_DB}" "${workload}" 4 "${ROWS}" 0 --mode single
    done
fi

# ----------------------------------------------------------
# [7/7] 内存消融 M0~M3（latency profile + P99 样本下限）
# ----------------------------------------------------------
if [[ "${DO_MEMORY}" -eq 1 ]]; then
    log "[7/7] memory ablation M0..M3 (latency profile, min_samples=${MIN_SAMPLES})"

    mem_m0=(--memory system --buffer-mode direct --block-cache 0  --buffer-cache 0)
    mem_m1=(--memory arena  --buffer-mode direct --block-cache 0  --buffer-cache 0)
    mem_m2=(--memory arena  --buffer-mode pooled --block-cache 64 --buffer-cache 0)
    mem_m3=(--memory arena  --buffer-mode pooled --block-cache 64 --buffer-cache 64)

    for workload in q3 q5 mixed; do
        for clients in 1 4; do
            echo "  workload=${workload} clients=${clients}"
            repeat_bench avx2 "${LATENCY_DB}" "${workload}" "${clients}" "${LATENCY_ROWS}" "${MIN_SAMPLES}" "${mem_m0[@]}"
            repeat_bench avx2 "${LATENCY_DB}" "${workload}" "${clients}" "${LATENCY_ROWS}" "${MIN_SAMPLES}" "${mem_m1[@]}"
            repeat_bench avx2 "${LATENCY_DB}" "${workload}" "${clients}" "${LATENCY_ROWS}" "${MIN_SAMPLES}" "${mem_m2[@]}"
            repeat_bench avx2 "${LATENCY_DB}" "${workload}" "${clients}" "${LATENCY_ROWS}" "${MIN_SAMPLES}" "${mem_m3[@]}"
        done
    done
fi

# ----------------------------------------------------------
# SIMD / allocator micro benchmark（只记录原始输出；失败立即中止）
# ----------------------------------------------------------
if [[ "${DO_MICRO}" -eq 1 ]]; then
    log "[extra] micro benchmarks"
    : > "${MICRO_LOG}"
    for micro in bench_compare_kernel bench_arith_kernel bench_sparse_projection bench_selection_pipeline \
                 bench_global_aggregate bench_perf_counters; do
        if [[ -x "${BUILD_DIR}/bin/${micro}" ]]; then
            {
                echo "----- ${micro} -----"
                if [[ -n "${TASKSET_CPUS}" ]]; then
                    taskset -c "${TASKSET_CPUS}" "${BUILD_DIR}/bin/${micro}"
                else
                    "${BUILD_DIR}/bin/${micro}"
                fi
                echo
            } | tee -a "${MICRO_LOG}"
        else
            echo "  [warn] ${micro} not built; skipped"
        fi
    done
fi

# ----------------------------------------------------------
# 汇总：按配置取 median（按 CSV 表头名定位列，避免列序变化）
# ----------------------------------------------------------
summarize() {
    awk -F',' '
        function median(values, n,    i, j, tmp) {
            for (i = 2; i <= n; ++i) {
                tmp = values[i];
                j = i - 1;
                while (j >= 1 && values[j] > tmp) { values[j + 1] = values[j]; --j; }
                values[j + 1] = tmp;
            }
            if (n % 2 == 1) return values[(n + 1) / 2];
            return (values[n / 2] + values[n / 2 + 1]) / 2.0;
        }
        NR == 1 {
            for (i = 1; i <= NF; ++i) col[$i] = i;
            next
        }
        {
            key = $(col["backend"]) "|" $(col["workload"]) "|" $(col["result_mode"]) "|" $(col["rows"]) "|" \
                  $(col["clients"]) "|" $(col["execution_mode"]) "|" $(col["scan_threads"]) "|" \
                  $(col["compute_threads"]) "|" $(col["memory_mode"]) "|" $(col["buffer_mode"]) "|" \
                  $(col["block_cache"]) "|" $(col["buffer_cache"]);
            ++count[key];
            qps[key SUBSEP count[key]] = $(col["qps"]) + 0;
            p50[key SUBSEP count[key]] = $(col["p50_us"]) + 0;
            p99[key SUBSEP count[key]] = $(col["p99_us"]) + 0;
        }
        END {
            printf "%-6s %-22s %-12s %9s %7s %-7s %5s %5s %-6s %-6s %5s %5s | %10s %10s %10s %5s\n",
                   "backend", "workload", "result", "rows", "clients", "exec", "scan", "comp", "memory", "buffer",
                   "blk$", "buf$", "qps_med", "p50_med", "p99_med", "runs";
            for (k in count) {
                n = count[k];
                for (i = 1; i <= n; ++i) { q[i] = qps[k SUBSEP i]; p5[i] = p50[k SUBSEP i]; p9[i] = p99[k SUBSEP i]; }
                split(k, parts, "|");
                printf "%-6s %-22s %-12s %9s %7s %-7s %5s %5s %-6s %-6s %5s %5s | %10.2f %10.0f %10.0f %5d\n",
                       parts[1], parts[2], parts[3], parts[4], parts[5], parts[6], parts[7], parts[8], parts[9],
                       parts[10], parts[11], parts[12], median(q, n), median(p5, n), median(p9, n), n;
            }
        }
    ' "${CSV}"
}

log "summary (median of ${REPEATS} runs per config)"
if [[ ! -f "${CSV}" ]]; then
    echo "no measurement CSV produced (all measurement phases skipped); nothing to summarize"
    exit 0
fi
SUMMARY_BODY="$(summarize)"
{
    echo "${SUMMARY_BODY}" | head -n 1
    echo "${SUMMARY_BODY}" | tail -n +2 | sort -k2,2 -k3,3 -k4,4n -k5,5n
} | tee "${SUMMARY}"

printf '\nCSV     : %s\n' "${CSV}"
printf 'Summary : %s\n' "${SUMMARY}"
if [[ "${DO_MICRO}" -eq 1 ]]; then
    printf 'Micro   : %s\n' "${MICRO_LOG}"
fi
