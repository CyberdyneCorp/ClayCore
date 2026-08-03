#pragma once

// Minimal persistent thread pool for batch dispatch. One global pool sized
// to hardware concurrency; parallel_for splits [0, n) into contiguous
// chunks. No per-item allocation.

#include <atomic>
#include <condition_variable>
#include <functional>
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
        std::atomic<std::size_t> next{0};
        std::atomic<std::size_t> done{0};
        {
            std::lock_guard<std::mutex> lock(mutex_);
            job_ = [&, chunk, n, num_tasks]() {
                for (;;) {
                    std::size_t idx = next.fetch_add(1);
                    if (idx >= num_tasks) break;
                    std::size_t b = idx * chunk;
                    std::size_t e = b + chunk < n ? b + chunk : n;
                    fn(b, e);
                    done.fetch_add(1);
                }
            };
            ++generation_;
        }
        cv_.notify_all();
        job_();  // calling thread participates
        while (done.load() < num_tasks) std::this_thread::yield();
        {
            std::lock_guard<std::mutex> lock(mutex_);
            job_ = nullptr;
        }
    }

  private:
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
            std::function<void()> job;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                cv_.wait(lock, [&] { return stop_ || (job_ && generation_ != seen); });
                if (stop_) return;
                seen = generation_;
                job = job_;
            }
            if (job) job();
        }
    }

    std::vector<std::thread> threads_;
    std::mutex mutex_;
    std::condition_variable cv_;
    std::function<void()> job_;
    std::uint64_t generation_ = 0;
    bool stop_ = false;
};

}  // namespace backends_cpu
}  // namespace clay
