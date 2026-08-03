#pragma once

// Minimal persistent thread pool for batch dispatch. One global pool sized
// to hardware concurrency; parallel_for splits [0, n) into contiguous
// chunks. Job state lives in a shared_ptr so a late-waking worker can never
// touch a completed call's stack (it just finds no chunks left).

#include <atomic>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace clay {
namespace backends_cpu {

class ThreadPool {
  public:
    static ThreadPool& instance() {
        static ThreadPool pool;
        return pool;
    }

    // fn(begin, end) is called on worker threads over disjoint ranges.
    void parallel_for(std::size_t n, std::size_t min_chunk,
                      const std::function<void(std::size_t, std::size_t)>& fn) {
        if (n == 0) return;
        std::size_t workers = threads_.size() + 1;  // + calling thread
        std::size_t chunk = (n + workers - 1) / workers;
        if (chunk < min_chunk) chunk = min_chunk;
        std::size_t num_tasks = (n + chunk - 1) / chunk;
        if (num_tasks <= 1) {
            fn(0, n);
            return;
        }

        auto job = std::make_shared<Job>();
        job->fn = fn;
        job->n = n;
        job->chunk = chunk;
        job->num_tasks = num_tasks;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            current_ = job;
            ++generation_;
        }
        cv_.notify_all();
        run(*job);  // calling thread participates
        // all chunks are claimed before done reaches num_tasks, so once done
        // equals num_tasks no worker can be inside fn
        while (job->done.load(std::memory_order_acquire) < num_tasks)
            std::this_thread::yield();
        {
            std::lock_guard<std::mutex> lock(mutex_);
            current_.reset();
        }
    }

  private:
    struct Job {
        std::function<void(std::size_t, std::size_t)> fn;
        std::size_t n = 0;
        std::size_t chunk = 0;
        std::size_t num_tasks = 0;
        std::atomic<std::size_t> next{0};
        std::atomic<std::size_t> done{0};
    };

    static void run(Job& job) {
        for (;;) {
            std::size_t idx = job.next.fetch_add(1, std::memory_order_relaxed);
            if (idx >= job.num_tasks) break;
            std::size_t b = idx * job.chunk;
            std::size_t e = b + job.chunk < job.n ? b + job.chunk : job.n;
            job.fn(b, e);
            job.done.fetch_add(1, std::memory_order_release);
        }
    }

    ThreadPool() {
        unsigned hc = std::thread::hardware_concurrency();
        unsigned count = hc > 1 ? hc - 1 : 0;
        for (unsigned i = 0; i < count; ++i)
            threads_.emplace_back([this] { worker(); });
    }
    ~ThreadPool() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stop_ = true;
        }
        cv_.notify_all();
        for (std::thread& t : threads_) t.join();
    }

    void worker() {
        std::uint64_t seen = 0;
        for (;;) {
            std::shared_ptr<Job> job;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                cv_.wait(lock, [&] { return stop_ || (current_ && generation_ != seen); });
                if (stop_) return;
                seen = generation_;
                job = current_;  // shared ownership: safe past completion
            }
            if (job) run(*job);
        }
    }

    std::vector<std::thread> threads_;
    std::mutex mutex_;
    std::condition_variable cv_;
    std::shared_ptr<Job> current_;
    std::uint64_t generation_ = 0;
    bool stop_ = false;
};

}  // namespace backends_cpu
}  // namespace clay
