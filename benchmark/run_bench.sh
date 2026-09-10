#!/usr/bin/env bash
#
# 构建并运行并行扫描 benchmark。
#
# 用法：
#   ./benchmark/run_bench.sh                  # 默认 400 万行，全场景
#   ./benchmark/run_bench.sh --rows 8000000 --threads 1,2,4,8
#   ./benchmark/run_bench.sh --scenario filter --repeats 7
#
# 所有参数透传给 bench_parallel_scan（--help 查看全部选项）。

set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${ROOT_DIR}/build"
BUILD_TYPE="${BUILD_TYPE:-Release}"
JOBS="$(nproc 2>/dev/null || echo 4)"

echo "[1/2] configure + build (${BUILD_TYPE}) ..."
cmake -S "${ROOT_DIR}" -B "${BUILD_DIR}" -DCMAKE_BUILD_TYPE="${BUILD_TYPE}" >/dev/null
cmake --build "${BUILD_DIR}" -j"${JOBS}" --target bench_parallel_scan

echo "[2/2] run baseline ..."
BIN="${BUILD_DIR}/bin/bench_parallel_scan"
if [[ ! -x "${BIN}" ]]; then
    echo "benchmark binary not found: ${BIN}" >&2
    exit 1
fi

exec "${BIN}" "$@"
