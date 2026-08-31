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

# ---------- 静态库：引擎核心 ----------
add_library(simple_olap_core STATIC ${PROJECT_SOURCES})
target_include_directories(simple_olap_core PUBLIC ${CMAKE_SOURCE_DIR}/src)

# Linux 下 mmap 由 libc 提供，无需额外链接；
# 若后续引入线程池并行，再补 find_package(Threads) 与 Threads::Threads。

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
