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

if(SIMPLE_OLAP_BUILD_BENCHMARK)
    add_executable(bench_parallel_scan ${CMAKE_SOURCE_DIR}/benchmark/bench_parallel_scan.cpp)
    target_link_libraries(bench_parallel_scan PRIVATE simple_olap_core)
    # 注入项目根目录，使默认数据目录与 CWD 无关
    target_compile_definitions(bench_parallel_scan PRIVATE
        SIMPLE_OLAP_BENCH_ROOT_DIR="${CMAKE_SOURCE_DIR}")
endif()
