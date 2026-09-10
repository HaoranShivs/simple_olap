#pragma once

#include <condition_variable>
#include <exception>
#include <mutex>
#include <queue>
#include <utility>

namespace simple_olap {

// 有界阻塞队列：在 storage scan 线程与 compute 线程之间传递 VectorBatch。
//
// 状态语义：
//   Push：队列满时阻塞；已 Close/Abort/Cancel 时返回 false（数据被丢弃）。
//   Pop ：队列空时阻塞；已 Close/Abort/Cancel 且队列为空时返回 false。
//   Close() ：正常 EOF（最后一个 scan worker 调用），唤醒所有等待线程。
//   Abort() ：扫描/执行异常，唤醒所有等待线程；可携带 exception_ptr，
//             由 Pop 侧通过 Error() 取出并 rethrow。
//   Cancel()：提前取消（析构或上层放弃），语义同 Abort 但不携带错误。
//
// T 需可移动（Push/Pop 均以 move 传递，不复制 batch）。
template <typename T> class BoundedBlockingQueue {
  public:
    explicit BoundedBlockingQueue(size_t capacity) : capacity_(capacity == 0 ? 1 : capacity) {}

    BoundedBlockingQueue(const BoundedBlockingQueue&) = delete;
    BoundedBlockingQueue& operator=(const BoundedBlockingQueue&) = delete;

    // ---------- 生产者 / 消费者 ----------

    // 生产者：以移动语义写入一个元素。队列已关闭/中止/取消时返回 false。
    bool Push(T&& value) {
        std::unique_lock<std::mutex> lock(mutex_);
        not_full_.wait(lock, [this] { return closed_ || aborted_ || cancelled_ || queue_.size() < capacity_; });
        if (closed_ || aborted_ || cancelled_) {
            return false;
        }
        queue_.push(std::move(value));
        lock.unlock();
        not_empty_.notify_one();
        return true;
    }

    // 消费者：取出一个元素。队列已空且关闭/中止/取消时返回 false。
    bool Pop(T& value) {
        std::unique_lock<std::mutex> lock(mutex_);
        not_empty_.wait(lock, [this] { return !queue_.empty() || closed_ || aborted_ || cancelled_; });
        if (queue_.empty()) {
            return false;
        }
        value = std::move(queue_.front());
        queue_.pop();
        lock.unlock();
        not_full_.notify_one();
        return true;
    }

    // ---------- 终止与状态 ----------

    // 正常 EOF：唤醒所有等待线程。
    void Close() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            closed_ = true;
        }
        not_empty_.notify_all();
        not_full_.notify_all();
    }

    // 异常中止：唤醒所有等待线程，可携带异常供消费侧 rethrow。
    void Abort(std::exception_ptr error = nullptr) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            aborted_ = true;
            if (error != nullptr) {
                error_ = std::move(error);
            }
        }
        not_empty_.notify_all();
        not_full_.notify_all();
    }

    // 提前取消：唤醒所有等待线程（不携带错误）。
    void Cancel() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            cancelled_ = true;
        }
        not_empty_.notify_all();
        not_full_.notify_all();
    }

    // 是否已调用 Close()。
    bool Closed() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return closed_;
    }

    // Abort 时携带的异常（无则返回 nullptr）。
    std::exception_ptr Error() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return error_;
    }

  private:
    // ---------- 成员变量 ----------

    size_t capacity_; // 队列容量上限（至少为 1）

    mutable std::mutex mutex_;
    std::condition_variable not_empty_; // 队列非空时唤醒消费者
    std::condition_variable not_full_;  // 队列未满时唤醒生产者
    std::queue<T> queue_;

    bool closed_ = false;    // 正常 EOF
    bool aborted_ = false;   // 异常中止
    bool cancelled_ = false; // 提前取消
    std::exception_ptr error_;
};

} // namespace simple_olap
