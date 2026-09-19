cmake_minimum_required(VERSION 3.16)

project(simple_olap
    VERSION 0.1.0
    DESCRIPTION "A columnar OLAP database engine with vectorized execution"
    LANGUAGES CXX)

# ---------- 全局编译标准 ----------
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# 默认构建类型：未指定时用 Release
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
    set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS Debug Release RelWithDebInfo MinSizeRel)
endif()

# 输出目录统一放到 build/bin 与 build/lib
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

# ---------- 收集源码 ----------
# 项目仍在快速迭代，用 GLOB_RECURSE 自动纳入 src 下所有 .cpp，
# 新增文件无需手动登记。CONFIGURE_DEPENDS 让文件变化时自动重新配置。
file(GLOB_RECURSE PROJECT_SOURCES CONFIGURE_DEPENDS
    ${CMAKE_SOURCE_DIR}/src/*.cpp)

# main.cpp 单独作为可执行入口，其余编译成静态库
list(FILTER PROJECT_SOURCES EXCLUDE REGEX ".*/main\\.cpp$")
# main_test.cpp 是 sql 模块的独立手工编译测试（自带 main()，见文件头编译说明），
# 不参与库构建
list(FILTER PROJECT_SOURCES EXCLUDE REGEX ".*/main_test\\.cpp$")

# ---------- SIMD：AVX2 源文件单独开启 -mavx2 ----------
# 只对 src/simd/avx2/*.cpp 生效；其余代码不要求 AVX2 指令集，
# 保证引擎在没有 AVX2 的 CPU 上也能构建与运行（运行时由 KernelRegistry
# 通过 __builtin_cpu_supports 在 scalar / AVX2 backend 间选择）。
file(GLOB AVX2_SOURCES CONFIGURE_DEPENDS
    ${CMAKE_SOURCE_DIR}/src/simd/avx2/*.cpp)

if(AVX2_SOURCES AND CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64|amd64|i[3-6]86)$")
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        set_source_files_properties(${AVX2_SOURCES}
            PROPERTIES COMPILE_OPTIONS "-mavx2")
    endif()
endif()

# ---------- 静态库：引擎核心 ----------
# 并行执行（线程池 / 阻塞队列 / future）依赖 pthread
find_package(Threads REQUIRED)

add_library(simple_olap_core STATIC ${PROJECT_SOURCES})
target_include_directories(simple_olap_core PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(simple_olap_core PUBLIC Threads::Threads)

# Release 优化：仅开 -O3。
# 不使用 -march=native（会强制本机指令集）与 -ffast-math（改变浮点语义）。
target_compile_options(simple_olap_core PRIVATE
    $<$<CONFIG:Release>:-O3>)

# ---------- 可执行：测试入口 ----------
add_executable(simple_olap ${CMAKE_SOURCE_DIR}/src/main.cpp)
target_link_libraries(simple_olap PRIVATE simple_olap_core)
# 注入项目根目录，使 main.cpp 中的 database 路径与 CWD 无关
target_compile_definitions(simple_olap PRIVATE
    SIMPLE_OLAP_ROOT_DIR="${CMAKE_SOURCE_DIR}")

# ---------- 可选：编译告警（Debug 下开启） ----------
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    target_compile_options(simple_olap_core PRIVATE -Wall -Wextra)
endif()

# ---------- 可选：benchmark 可执行 ----------
# 默认开启；用 -DSIMPLE_OLAP_BUILD_BENCHMARK=OFF 关闭。
# 并行扫描 vs 串行扫描运行时间对比。
option(SIMPLE_OLAP_BUILD_BENCHMARK "Build benchmark executables" ON)

# benchmark 环境信息：commit SHA + build type（bench_common.h 使用）
execute_process(
    COMMAND git rev-parse --short HEAD
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    OUTPUT_VARIABLE SIMPLE_OLAP_GIT_COMMIT
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET)
if(NOT SIMPLE_OLAP_GIT_COMMIT)
    set(SIMPLE_OLAP_GIT_COMMIT "unknown")
endif()

# 给每个 benchmark 注入环境宏；static 目标一次即可
function(simple_olap_configure_bench target)
    target_compile_definitions(${target} PRIVATE
        SIMPLE_OLAP_GIT_COMMIT="${SIMPLE_OLAP_GIT_COMMIT}"
        SIMPLE_OLAP_BUILD_TYPE="${CMAKE_BUILD_TYPE}")
endfunction()

if(SIMPLE_OLAP_BUILD_BENCHMARK)
    add_executable(bench_parallel_scan ${CMAKE_SOURCE_DIR}/benchmark/bench_parallel_scan.cpp)
    target_link_libraries(bench_parallel_scan PRIVATE simple_olap_core)
    # 注入项目根目录，使默认数据目录与 CWD 无关
    target_compile_definitions(bench_parallel_scan PRIVATE
        SIMPLE_OLAP_BENCH_ROOT_DIR="${CMAKE_SOURCE_DIR}")
    simple_olap_configure_bench(bench_parallel_scan)

    # Projection 算术内核微基准（scalar vs AVX2 vs 旧逐行 variant 路径）
    add_executable(bench_arith_kernel ${CMAKE_SOURCE_DIR}/benchmark/bench_arith_kernel.cpp)
    target_link_libraries(bench_arith_kernel PRIVATE simple_olap_core)
    simple_olap_configure_bench(bench_arith_kernel)

    # Sparse projection 微基准（mask -> gather -> SIMD vs 旧逐行物化）
    add_executable(bench_sparse_projection ${CMAKE_SOURCE_DIR}/benchmark/bench_sparse_projection.cpp)
    target_link_libraries(bench_sparse_projection PRIVATE simple_olap_core)
    simple_olap_configure_bench(bench_sparse_projection)

    # Selection pipeline 微基准（mask-native vs selection-vector 往返）
    add_executable(bench_selection_pipeline ${CMAKE_SOURCE_DIR}/benchmark/bench_selection_pipeline.cpp)
    target_link_libraries(bench_selection_pipeline PRIVATE simple_olap_core)
    simple_olap_configure_bench(bench_selection_pipeline)

    # AVX2 compare 微基准（scalar / 64-row bitmap block / 旧逐段 |= 写法）
    add_executable(bench_compare_kernel ${CMAKE_SOURCE_DIR}/benchmark/bench_compare_kernel.cpp)
    target_link_libraries(bench_compare_kernel PRIVATE simple_olap_core)
    simple_olap_configure_bench(bench_compare_kernel)

    # Global aggregate 微基准（hash 表路径 vs Global Fast Path）
    add_executable(bench_global_aggregate ${CMAKE_SOURCE_DIR}/benchmark/bench_global_aggregate.cpp)
    target_link_libraries(bench_global_aggregate PRIVATE simple_olap_core)
    simple_olap_configure_bench(bench_global_aggregate)

    # Hardware counter 微基准（perf_event_open：cycles / IPC / LLC miss / branch miss）
    add_executable(bench_perf_counters ${CMAKE_SOURCE_DIR}/benchmark/bench_perf_counters.cpp)
    target_link_libraries(bench_perf_counters PRIVATE simple_olap_core)
    simple_olap_configure_bench(bench_perf_counters)

    # 端到端查询 benchmark（engine / materialized QPS + P50/P95/P99 + 内存消融）
    add_executable(bench_query_workload ${CMAKE_SOURCE_DIR}/benchmark/bench_query_workload.cpp)
    target_link_libraries(bench_query_workload PRIVATE simple_olap_core)
    target_compile_definitions(bench_query_workload PRIVATE
        SIMPLE_OLAP_BENCH_ROOT_DIR="${CMAKE_SOURCE_DIR}")
    simple_olap_configure_bench(bench_query_workload)
endif()

# ---------- 可选：单元测试 ----------
# 默认开启；用 -DSIMPLE_OLAP_BUILD_TESTS=OFF 关闭。
option(SIMPLE_OLAP_BUILD_TESTS "Build test executables" ON)

if(SIMPLE_OLAP_BUILD_TESTS)
    # SelectionMask 不变式 + ForEachSetBit / SelectionCursor + compare kernel 一致性
    add_executable(simd_compare_test ${CMAKE_SOURCE_DIR}/test/simd_compare_test.cpp)
    target_link_libraries(simd_compare_test PRIVATE simple_olap_core)

    # typed gather：scalar vs AVX2，覆盖各类型 / 选择率
    add_executable(gather_kernel_test ${CMAKE_SOURCE_DIR}/test/gather_kernel_test.cpp)
    target_link_libraries(gather_kernel_test PRIVATE simple_olap_core)

    # mask-native 执行链：Filter / Projection / Aggregate 的 dense + sparse 路径
    add_executable(mask_pipeline_test ${CMAKE_SOURCE_DIR}/test/mask_pipeline_test.cpp)
    target_link_libraries(mask_pipeline_test PRIVATE simple_olap_core)

    # Global aggregate：空表 / 过滤选择率 / expression / GROUP BY / single==multi
    add_executable(global_aggregate_test ${CMAKE_SOURCE_DIR}/test/global_aggregate_test.cpp)
    target_link_libraries(global_aggregate_test PRIVATE simple_olap_core)
endif()
