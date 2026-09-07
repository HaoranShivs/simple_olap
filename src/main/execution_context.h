#pragma once

#include <cstddef>
#include <filesystem>
#include <thread>

namespace simple_olap {

struct DatabaseConfig {
    size_t thread_count = [] {
        const auto n = std::thread::hardware_concurrency();
        return n == 0 ? 1 : static_cast<size_t>(n);
    }();

    size_t query_arena_initial_size = 1 << 20; // 1 MB
};

}