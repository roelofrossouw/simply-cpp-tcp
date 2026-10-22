#include "tcp_worker_pool.h"

#include <algorithm>

namespace sc::tcp {
    bool strand::post(std::function<void()> task) {
        const std::lock_guard lock{mutex_};
        tasks_.push_back(std::move(task));
        if (scheduled_) return false;
        scheduled_ = true;
        return true;
    }

    void strand::drain() {
        while (true) {
            std::function<void()> task;
            {
                const std::lock_guard lock{mutex_};
                if (tasks_.empty()) {
                    scheduled_ = false;
                    return;
                }
                task = std::move(tasks_.front());
                tasks_.pop_front();
            }
            task();
        }
    }

    worker_pool::worker_pool(std::size_t count) {
        if (count == 0) count = std::max<std::size_t>(2, std::thread::hardware_concurrency());
        for (std::size_t i = 0; i < count; ++i) threads_.emplace_back([this] { work(); });
    }

    worker_pool::~worker_pool() {
        finish();
    }

    void worker_pool::post(const std::shared_ptr<strand> &on, std::function<void()> task) {
        if (!on->post(std::move(task))) return;
        {
            const std::lock_guard lock{mutex_};
            queue_.push_back(on);
        }
        ready_.notify_one();
    }

    void worker_pool::finish() {
        {
            const std::lock_guard lock{mutex_};
            if (stopping_) return;
            stopping_ = true;
        }
        ready_.notify_all();
        for (auto &t: threads_)
            if (t.joinable()) t.join();
    }

    void worker_pool::work() {
        while (true) {
            std::shared_ptr<strand> next;
            {
                std::unique_lock lock{mutex_};
                ready_.wait(lock, [this] { return stopping_ || !queue_.empty(); });
                if (queue_.empty()) return; // stopping, and nothing left
                next = std::move(queue_.front());
                queue_.pop_front();
            }
            next->drain();
        }
    }
}
