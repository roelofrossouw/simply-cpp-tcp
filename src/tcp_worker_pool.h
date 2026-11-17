#pragma once

#include <condition_variable>
#include <cstddef>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

// Worker threads, and strands: a strand's tasks run one at a time and in order, on whichever
// worker is free; different strands' run side by side. Each tcp_connection is a strand.
namespace sc::tcp {
    class strand {
    public:
        virtual ~strand() = default;
        // Queues a task; true when the strand isn't waiting for a worker yet (schedule it then).
        bool post(std::function<void()> task);
        // Runs the tasks until none are left (a worker's job).
        void drain();

    private:
        std::mutex mutex_;
        std::deque<std::function<void()>> tasks_;
        bool scheduled_ = false;
    };

    class worker_pool {
    public:
        explicit worker_pool(std::size_t count); // 0: one a CPU, at least 2
        ~worker_pool();
        worker_pool(const worker_pool &) = delete;
        worker_pool &operator=(const worker_pool &) = delete;

        // A task on the strand, running it once a worker is free.
        void post(const std::shared_ptr<strand> &on, std::function<void()> task);
        // Runs what's queued, then stops the workers.
        void finish();

    private:
        void work();

        std::mutex mutex_;
        std::condition_variable ready_;
        std::deque<std::shared_ptr<strand>> queue_;
        std::vector<std::thread> threads_;
        bool stopping_ = false;
    };
}
