#pragma once

#include <condition_variable>
#include <functional>
#include <future>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

namespace simple_olap {

// 最小线程池实现：供执行层并行扫描使用。
// 后续可按需扩展（优先级队列、亲和性、动态扩缩容等）。
class ThreadPool {
  public:
    explicit ThreadPool(size_t thread_count = 0) {
        if (thread_count == 0) {
            thread_count = std::max(1u, std::thread::hardware_concurrency());
        }
        workers_.reserve(thread_count);
        for (size_t i = 0; i < thread_count; ++i) {
            workers_.emplace_back([this] { WorkerLoop(); });
        }
    }

    ~ThreadPool() {
        Shutdown();
    }

    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;

    // 提交任务，返回 future 以获取结果
    template <typename F> auto Submit(F&& task) -> std::future<decltype(task())> {
        using ReturnType = decltype(task());
        auto bound = std::make_shared<std::packaged_task<ReturnType()>>(std::forward<F>(task));
        std::future<ReturnType> result = bound->get_future();
        {
            std::lock_guard<std::mutex> lock(mutex_);
            tasks_.emplace([bound] { (*bound)(); });
        }
        cv_.notify_one();
        return result;
    }

    size_t thread_count() const noexcept {
        return workers_.size();
    }

    void Shutdown() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (stopped_) {
                return;
            }
            stopped_ = true;
        }
        cv_.notify_all();
        for (auto& worker : workers_) {
            if (worker.joinable()) {
                worker.join();
            }
        }
        workers_.clear();
    }

  private:
    void WorkerLoop() {
        for (;;) {
            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait(lock, [this] { return stopped_ || !tasks_.empty(); });
            if (stopped_ && tasks_.empty()) {
                return;
            }
            auto task = std::move(tasks_.front());
            tasks_.pop();
            lock.unlock();
            task();
        }
    }

    std::vector<std::thread> workers_;
    std::queue<std::function<void()>> tasks_;
    std::mutex mutex_;
    std::condition_variable cv_;
    bool stopped_ = false;
};

} // namespace simple_olap
