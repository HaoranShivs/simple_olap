// ============================================================
// Hardware Counter 微基准：cycles / instructions / cache / branch
// ============================================================
//
// 通过 perf_event_open（Linux）统计标量 vs AVX2 内核的：
//
//   cycles, instructions, IPC
//   L1-dcache-loads / misses
//   LLC-loads / misses
//   branches / branch-misses
//
// 主指标按 BenchmarkSettingV2 §18 定义：
//
//   LLC miss rate = LLC-load-misses / LLC-loads
//   LLC MPKI      = LLC-load-misses / instructions * 1000
//
// 并报告 branch miss rate。单一 raw miss 数在 query 时长变化后不可比，
// 因此必须和 loads / instructions 一起看。
//
// 测量对象：compare i32 与 arith f64 的 scalar / AVX2 内核；
// 每个 kernel 重复 --trials 次，按轮次交替 scalar/AVX2 顺序，取 derived
// 指标的 median（计数本身受频率与调度影响较大，比率更稳定）。
//
// 构建：
//   cmake --build build --target bench_perf_counters
// 运行：
//   ./build/bin/bench_perf_counters [--inner N] [--trials N]
//
// 若 perf_event_open 不可用（无权限 / 非 Linux），程序打印说明并退出 0，
// 不使 benchmark suite 失败。

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "../src/simd/kernels.h"
#include "../src/simd/selection_mask.h"
#include "../src/type.h"
#include "bench_common.h"

#if defined(__linux__) && (defined(__x86_64__) || defined(__aarch64__) || defined(__i386__))
#include <linux/perf_event.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <unistd.h>
#define BENCH_HAS_PERF_EVENTS 1
#else
#define BENCH_HAS_PERF_EVENTS 0
#endif

using namespace simple_olap;
using namespace simple_olap::simd;
using namespace simple_olap::bench;

namespace {

constexpr uint32_t kN = 1024;

struct Args {
    uint64_t inner = 20000;
    uint32_t trials = 5;
};

Args ParseArgs(int argc, char** argv) {
    Args a;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        const auto next = [&]() -> std::string {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "missing value for %s\n", arg.c_str());
                std::exit(1);
            }
            return argv[++i];
        };
        if (arg == "--inner") {
            a.inner = std::stoull(next());
        } else if (arg == "--trials") {
            a.trials = static_cast<uint32_t>(std::stoul(next()));
        } else {
            std::fprintf(stderr, "unknown argument: %s\n", arg.c_str());
            std::exit(1);
        }
    }
    return a;
}

int32_t g_i32[kN];
double g_f64[kN];
double g_out[kN];
SelectionMask g_mask;

#if BENCH_HAS_PERF_EVENTS

enum EventIndex : size_t {
    kCycles = 0,
    kInstructions,
    kBranches,
    kBranchMisses,
    kL1dLoads,
    kL1dLoadMisses,
    kLlcLoads,
    kLlcLoadMisses,
    kCacheReferences,
    kCacheMisses,
    kEventCount,
};

constexpr uint64_t HwCache(uint64_t cache, uint64_t op, uint64_t result) {
    return cache | (op << 8) | (result << 16);
}

struct EventSpec {
    const char* name;
    uint32_t type;
    uint64_t config;
};

constexpr EventSpec kEvents[kEventCount] = {
    {"cycles", PERF_TYPE_HARDWARE, PERF_COUNT_HW_CPU_CYCLES},
    {"instructions", PERF_TYPE_HARDWARE, PERF_COUNT_HW_INSTRUCTIONS},
    {"branches", PERF_TYPE_HARDWARE, PERF_COUNT_HW_BRANCH_INSTRUCTIONS},
    {"branch-misses", PERF_TYPE_HARDWARE, PERF_COUNT_HW_BRANCH_MISSES},
    {"L1-dcache-loads", PERF_TYPE_HW_CACHE,
     HwCache(PERF_COUNT_HW_CACHE_L1D, PERF_COUNT_HW_CACHE_OP_READ, PERF_COUNT_HW_CACHE_RESULT_ACCESS)},
    {"L1-dcache-load-misses", PERF_TYPE_HW_CACHE,
     HwCache(PERF_COUNT_HW_CACHE_L1D, PERF_COUNT_HW_CACHE_OP_READ, PERF_COUNT_HW_CACHE_RESULT_MISS)},
    {"LLC-loads", PERF_TYPE_HW_CACHE,
     HwCache(PERF_COUNT_HW_CACHE_LL, PERF_COUNT_HW_CACHE_OP_READ, PERF_COUNT_HW_CACHE_RESULT_ACCESS)},
    {"LLC-load-misses", PERF_TYPE_HW_CACHE,
     HwCache(PERF_COUNT_HW_CACHE_LL, PERF_COUNT_HW_CACHE_OP_READ, PERF_COUNT_HW_CACHE_RESULT_MISS)},
    {"cache-references", PERF_TYPE_HARDWARE, PERF_COUNT_HW_CACHE_REFERENCES},
    {"cache-misses", PERF_TYPE_HARDWARE, PERF_COUNT_HW_CACHE_MISSES},
};

struct RawCounters {
    std::array<uint64_t, kEventCount> values{};
    std::array<bool, kEventCount> valid{};
};

class PerfSession {
  public:
    bool Open() {
        bool any = false;
        for (size_t i = 0; i < kEventCount; ++i) {
            perf_event_attr attr{};
            attr.type = kEvents[i].type;
            attr.size = sizeof(attr);
            attr.config = kEvents[i].config;
            attr.disabled = 1;
            attr.exclude_kernel = 1;
            attr.exclude_hv = 1;
            attr.inherit = 0;

            const long fd = syscall(__NR_perf_event_open, &attr, 0, -1, -1, 0);
            fds_[i] = (fd >= 0) ? static_cast<int>(fd) : -1;
            any = any || (fds_[i] >= 0);
        }
        return any;
    }

    ~PerfSession() {
        for (int fd : fds_) {
            if (fd >= 0) {
                ::close(fd);
            }
        }
    }

    // 执行 fn(inner) 一次并读取全部可用计数器。
    template <typename Fn> RawCounters Measure(Fn&& fn, uint64_t inner) {
        for (int fd : fds_) {
            if (fd >= 0) {
                ioctl(fd, PERF_EVENT_IOC_RESET, 0);
                ioctl(fd, PERF_EVENT_IOC_ENABLE, 0);
            }
        }

        fn(inner);

        for (int fd : fds_) {
            if (fd >= 0) {
                ioctl(fd, PERF_EVENT_IOC_DISABLE, 0);
            }
        }

        RawCounters counters;
        for (size_t i = 0; i < kEventCount; ++i) {
            if (fds_[i] < 0) {
                continue;
            }
            uint64_t value = 0;
            const ssize_t bytes = ::read(fds_[i], &value, sizeof(value));
            if (bytes == static_cast<ssize_t>(sizeof(value))) {
                counters.values[i] = value;
                counters.valid[i] = true;
            }
        }
        return counters;
    }

    void PrintUnavailable() const {
        for (size_t i = 0; i < kEventCount; ++i) {
            if (fds_[i] < 0) {
                std::printf("  [n/a] %s\n", kEvents[i].name);
            }
        }
    }

    bool valid(size_t index) const {
        return fds_[index] >= 0;
    }

  private:
    std::array<int, kEventCount> fds_{};
};

struct Derived {
    double cycles_per_op = 0.0;
    double instr_per_op = 0.0;
    double ipc = 0.0;
    double l1d_miss_rate = 0.0;
    double llc_miss_rate = 0.0;
    double llc_mpki = 0.0;
    double branch_miss_rate = 0.0;
};

Derived ComputeDerived(const RawCounters& c, double ops) {
    Derived d;
    if (c.valid[kCycles] && ops > 0.0) {
        d.cycles_per_op = static_cast<double>(c.values[kCycles]) / ops;
    }
    if (c.valid[kInstructions] && ops > 0.0) {
        d.instr_per_op = static_cast<double>(c.values[kInstructions]) / ops;
    }
    if (c.valid[kInstructions] && c.valid[kCycles] && c.values[kCycles] > 0) {
        d.ipc = static_cast<double>(c.values[kInstructions]) / static_cast<double>(c.values[kCycles]);
    }
    if (c.valid[kL1dLoads] && c.valid[kL1dLoadMisses] && c.values[kL1dLoads] > 0) {
        d.l1d_miss_rate =
            static_cast<double>(c.values[kL1dLoadMisses]) / static_cast<double>(c.values[kL1dLoads]);
    }
    if (c.valid[kLlcLoads] && c.valid[kLlcLoadMisses] && c.values[kLlcLoads] > 0) {
        d.llc_miss_rate = static_cast<double>(c.values[kLlcLoadMisses]) / static_cast<double>(c.values[kLlcLoads]);
    }
    if (c.valid[kInstructions] && c.valid[kLlcLoadMisses] && c.values[kInstructions] > 0) {
        d.llc_mpki =
            static_cast<double>(c.values[kLlcLoadMisses]) * 1000.0 / static_cast<double>(c.values[kInstructions]);
    }
    if (c.valid[kBranches] && c.valid[kBranchMisses] && c.values[kBranches] > 0) {
        d.branch_miss_rate =
            static_cast<double>(c.values[kBranchMisses]) / static_cast<double>(c.values[kBranches]);
    }
    return d;
}

std::string FormatOrNa(bool valid, double value, const char* fmt) {
    char buffer[64];
    if (!valid) {
        return "n/a";
    }
    std::snprintf(buffer, sizeof(buffer), fmt, value);
    return buffer;
}

#endif // BENCH_HAS_PERF_EVENTS

} // namespace

int main(int argc, char** argv) {
    const Args args = ParseArgs(argc, argv);

    std::printf("=== Hardware counter 微基准 ===\n");
    PrintEnvironment(QueryEnvironment());
    std::printf("inner = %llu, trials = %u\n\n", static_cast<unsigned long long>(args.inner), args.trials);

#if !BENCH_HAS_PERF_EVENTS
    std::printf("perf_event_open 不可用（非 Linux 或未编译支持）；跳过。\n");
    return 0;
#else
    PerfSession session;
    if (!session.Open()) {
        std::printf("perf_event_open 不可用（权限不足或内核未开启）；跳过。\n"
                    "提示：/proc/sys/kernel/perf_event_paranoid 需允许 per-process 计数。\n");
        return 0;
    }
    session.PrintUnavailable();

    CompareKernels scalar_compare;
    InstallScalarCompareKernels(scalar_compare);
    CompareKernels avx2_compare;
    InstallScalarCompareKernels(avx2_compare);
    InstallAvx2CompareKernels(avx2_compare);

    ArithmeticKernels scalar_arith;
    InstallScalarArithmeticKernels(scalar_arith);
    ArithmeticKernels avx2_arith;
    InstallScalarArithmeticKernels(avx2_arith);
    InstallAvx2ArithmeticKernels(avx2_arith);

    for (uint32_t i = 0; i < kN; ++i) {
        g_i32[i] = static_cast<int32_t>(i) * 7 - 3000;
        g_f64[i] = static_cast<double>(i) * 0.25 - 64.0;
    }

    struct Kernel {
        const char* name;
        std::function<void()> op;
    };

    const std::vector<Kernel> kernels = {
        {"compare-i32 scalar", [&] { scalar_compare.i32_const(g_i32, kN, CmpOp::GT, 1234, g_mask); }},
        {"compare-i32 avx2  ", [&] { avx2_compare.i32_const(g_i32, kN, CmpOp::GT, 1234, g_mask); }},
        {"arith-f64   scalar", [&] { scalar_arith.f64_const(ArithmeticOp::ADD, g_f64, 0.25, false, g_out, kN); }},
        {"arith-f64   avx2  ", [&] { avx2_arith.f64_const(ArithmeticOp::ADD, g_f64, 0.25, false, g_out, kN); }},
    };

    std::printf("%-20s %12s %12s %7s %10s %12s %12s %10s\n", "kernel", "cycles/op", "instr/op", "IPC",
                "L1d-miss", "LLC-miss", "LLC-MPKI", "br-miss");
    std::printf("%s\n", std::string(106, '-').c_str());

    // 交错顺序：奇数 trial 反向遍历，消除固定 A/B 顺序偏差。
    std::vector<std::vector<Derived>> samples(kernels.size());
    for (uint32_t trial = 0; trial < args.trials; ++trial) {
        const bool forward = (trial % 2 == 0);
        for (size_t step = 0; step < kernels.size(); ++step) {
            const size_t index = forward ? step : kernels.size() - 1 - step;
            const RawCounters counters = session.Measure(
                [&](uint64_t inner) {
                    for (uint64_t i = 0; i < inner; ++i) {
                        kernels[index].op();
                    }
                    DoNotOptimize(g_mask.data()[0]);
                    DoNotOptimize(g_out[0]);
                },
                args.inner);
            samples[index].push_back(ComputeDerived(counters, static_cast<double>(args.inner) * kN));
        }
    }

    for (size_t index = 0; index < kernels.size(); ++index) {
        std::vector<double> cycles_per_op, instr_per_op, ipc, l1d, llc, mpki, br;
        for (const Derived& d : samples[index]) {
            cycles_per_op.push_back(d.cycles_per_op);
            instr_per_op.push_back(d.instr_per_op);
            ipc.push_back(d.ipc);
            l1d.push_back(d.l1d_miss_rate);
            llc.push_back(d.llc_miss_rate);
            mpki.push_back(d.llc_mpki);
            br.push_back(d.branch_miss_rate);
        }

        const bool cycles_valid = session.valid(kCycles);
        const bool instr_valid = session.valid(kInstructions);
        const bool l1d_valid = session.valid(kL1dLoads) && session.valid(kL1dLoadMisses);
        const bool llc_valid = session.valid(kLlcLoads) && session.valid(kLlcLoadMisses);
        const bool mpki_valid = instr_valid && session.valid(kLlcLoadMisses);
        const bool branch_valid = session.valid(kBranches) && session.valid(kBranchMisses);

        std::printf("%-20s %12s %12s %7s %10s %12s %12s %10s\n", kernels[index].name,
                    FormatOrNa(cycles_valid, MedianOf(cycles_per_op), "%.2f").c_str(),
                    FormatOrNa(instr_valid, MedianOf(instr_per_op), "%.2f").c_str(),
                    FormatOrNa(cycles_valid && instr_valid, MedianOf(ipc), "%.2f").c_str(),
                    FormatOrNa(l1d_valid, MedianOf(l1d), "%.4f").c_str(),
                    FormatOrNa(llc_valid, MedianOf(llc), "%.4f").c_str(),
                    FormatOrNa(mpki_valid, MedianOf(mpki), "%.2f").c_str(),
                    FormatOrNa(branch_valid, MedianOf(br), "%.4f").c_str());
    }

    return 0;
#endif
}
