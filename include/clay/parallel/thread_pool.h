#pragma once

// THE ONE DATA-PARALLEL PRIMITIVE, and where it lives is the point.
//
// This used to be backends/cpu/thread_pool.h — a private header of the CPU
// backend, reached by the C bindings through a `../../backends/cpu/` relative
// include because there was no other way to name it. The layering rule is that
// no module depends on a backend (tools/check_layering.py), so the core library
// could not use the pool at all: every mesher, every voxel verb, redistance,
// the mask distance transform and the per-brick cull were single-threaded not
// because they resist parallelism but because they could not legally reach the
// only pool in the tree.
//
// It sits below everything now, in its own module that depends on nothing but
// the standard library, so a core module can use it and the layering gate can
// SEE that it does. That is the whole of this module's reason to exist.
//
// It is NOT a task system: no futures, no dependencies, no work stealing
// between unrelated jobs. One primitive — split [0, n) into contiguous chunks
// and run them — because that is the shape every consumer here has, and the
// house rule they all obey is disjoint output slices through the same scalar
// arithmetic, so the pool CHANGES SPEED AND NOTHING ELSE.
//
// Minimal persistent thread pool for batch dispatch. One global pool sized
// to hardware concurrency; parallel_for splits [0, n) into contiguous
// chunks. Job state lives in a shared_ptr so a late-waking worker can never
// touch a completed call's stack (it just finds no chunks left).
//
// TWO PROPERTIES THIS FILE OWES ITS CALLERS, both of which cost more than they
// look and are stated here because the code that implements them is small
// enough to read as incidental.
//
// NESTED CALLS RUN INLINE. There is one `current_` job slot, so a
// parallel_for issued from inside a worker would REPLACE the job that worker
// is running: the outer job stops being advertised, every worker that has not
// yet woken for it never will, and its remaining chunks fall to whichever
// threads are already inside it — usually one. Not a deadlock, which is what
// makes it dangerous: it is a silent collapse to serial that looks exactly
// like parallelism that did not help. A nested call therefore runs on the
// calling thread and does not touch the pool at all.
//
// This is a guard placed BEFORE the first nested caller rather than a fix for
// an observed bug. Nothing nests today — consolidate's injected evaluator is
// the only pooled call under another operation and that operation assembles
// serially — but the moment a second subsystem adopts the pool (brick meshing
// over independent bricks, whose per-brick work evaluates through it) two
// layers stack and this is what stops the second one from eating the first.
//
// A THREAD WITH NO WORK LEFT SLEEPS. The join used to spin on yield() until
// the stragglers finished, on the thread the caller is waiting on — which on
// an interactive path is the thread a user is waiting on. It blocks on a
// condition variable now. The guarantee the spin provided is unchanged and is
// the reason the wait is written the way it is: once `done` reaches
// `num_tasks`, every chunk has been claimed AND returned, so no worker is
// inside `fn`.

#include <atomic>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "clay/parallel/work_class.h"

namespace clay {
namespace parallel {

class ThreadPool {
  public:
    static ThreadPool& instance() {
        static ThreadPool pool;
        return pool;
    }

    // fn(begin, end) is called on worker threads over disjoint ranges.
    //
    // The class-less overload is the one every existing call site uses and it
    // keeps compiling unchanged. It takes the class THIS THREAD IS ALREADY
    // RUNNING AS, which is UserInitiated until something says otherwise — so an
    // unclassified dispatch on an unclassified thread behaves exactly as it did
    // before, and no call site silently became Interactive or Background.
    //
    // TAKING IT FROM THE THREAD RATHER THAN HARDCODING IT IS THE WHOLE POINT.
    // Almost every dispatch in this library is in a LEAF: `eval_points` in the
    // CPU backend, a marching wave, a volume bake. Those are reached from an
    // interactive dab and from a background rebuild alike, and annotating them
    // would have to pick one and be wrong for the other caller — which, since
    // the leaves are the hot paths, means marking everything Interactive and
    // arriving back where this started. The operation at the top knows what it
    // is for; the leaf underneath it does not and should not have to.
    void parallel_for(std::size_t n, std::size_t min_chunk,
                      const std::function<void(std::size_t, std::size_t)>& fn) {
        parallel_for(n, min_chunk, fn, current_work_class());
    }

    void parallel_for(std::size_t n, std::size_t min_chunk,
                      const std::function<void(std::size_t, std::size_t)>& fn, WorkClass cls) {
        if (n == 0) return;
        // Nested: run it here. See the note at the top of this file — going to
        // the pool would evict the job this thread is already running.
        //
        // IT INHERITS THE CALLER'S CLASS AND IGNORES `cls`. This runs on a
        // thread that is already inside somebody else's job, and lowering that
        // thread's class mid-job would slow the OUTER work — a Utility helper
        // called from an Interactive dab would drag the dab down with it. The
        // requested class is honoured when the call is a top-level one.
        if (in_job()) {
            fn(0, n);
            return;
        }
        std::size_t workers = threads_.size() + 1;  // + calling thread
        // One chunk per worker meant the atomic claim counter in run() never
        // rebalanced: each thread took exactly one chunk and a thread that
        // finished early parked instead of picking up more. The cores are not
        // interchangeable — a phone or tablet SoC pairs fast cores with
        // efficiency cores, and the OS may also be running something else on
        // one of them — so the call took as long as its slowest chunk.
        //
        // Over-decomposing gives the counter something to balance with. The
        // extra chunks are close to free: dispatch cost is dominated by the
        // wakeup, which is per CALL rather than per chunk, and each chunk is
        // one more fetch_add. min_chunk still floors it, so a small batch is
        // unaffected and work per chunk stays worth the claim.
        constexpr std::size_t kChunksPerWorker = 8;
        std::size_t chunk = (n + workers * kChunksPerWorker - 1) / (workers * kChunksPerWorker);
        if (chunk < min_chunk) chunk = min_chunk;
        if (chunk == 0) chunk = 1;
        std::size_t num_tasks = (n + chunk - 1) / chunk;
        if (num_tasks <= 1) {
            // A top-level call small enough to stay here still ASKED for a
            // class, and the thread it runs on is the one that will do the
            // work. Same scope the workers get, so a batch that crosses the
            // threshold does not change how it is scheduled.
            WorkClassScope scope(cls);
            fn(0, n);
            return;
        }

        auto job = std::make_shared<Job>();
        job->fn = fn;
        job->n = n;
        job->chunk = chunk;
        job->num_tasks = num_tasks;
        job->cls = cls;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            current_ = job;
            ++generation_;
        }
        cv_.notify_all();
        run(*job);  // calling thread participates
        // All chunks are claimed before done reaches num_tasks, so once done
        // equals num_tasks no worker can be inside fn. The calling thread has
        // run out of chunks to claim by the time it gets here; it waits for
        // the stragglers rather than burning a core yielding at them.
        //
        // No lost wakeup: the finisher takes `job->mutex` before notifying, and
        // this thread holds it from the predicate check until wait() releases
        // it, so a notify cannot land in between.
        {
            std::unique_lock<std::mutex> lock(job->mutex);
            job->finished.wait(lock, [&] {
                return job->done.load(std::memory_order_acquire) >= num_tasks;
            });
        }
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
        // What the workers should be scheduled as while they run this job.
        WorkClass cls = WorkClass::UserInitiated;
        std::atomic<std::size_t> next{0};
        std::atomic<std::size_t> done{0};
        // Waited on by the thread that issued the job, signalled by whichever
        // thread finishes the last chunk. In the Job rather than the pool so a
        // late worker signalling a completed call touches state the caller's
        // shared_ptr is still keeping alive.
        std::mutex mutex;
        std::condition_variable finished;
    };

    // True while this thread is inside a job's `fn`, on a worker or on the
    // thread that issued the job — both can reach user code that calls back in.
    static bool& in_job() {
        static thread_local bool active = false;
        return active;
    }

    // Sets the nesting flag for the duration of a scope and restores what it
    // was, rather than clearing it: a nested inline call must not tell the
    // frame above it that the pool is free again.
    struct InJobScope {
        bool previous;
        InJobScope() : previous(in_job()) { in_job() = true; }
        ~InJobScope() { in_job() = previous; }
    };

    static void run(Job& job) {
        // ADOPTED PER JOB, AND RESTORED AFTER. The workers are persistent, so a
        // class set and left would leak into whatever job this thread picks up
        // next — a Utility rebuild would keep running as Interactive because an
        // earlier dab happened to land on this worker. The calling thread goes
        // through here too, which is how the issuer's participation is
        // scheduled the same as the workers it is waiting on.
        WorkClassScope qos(job.cls);
        InJobScope scope;
        for (;;) {
            std::size_t idx = job.next.fetch_add(1, std::memory_order_relaxed);
            if (idx >= job.num_tasks) break;
            std::size_t b = idx * job.chunk;
            std::size_t e = b + job.chunk < job.n ? b + job.chunk : job.n;
            job.fn(b, e);
            // The thread that lands the last chunk wakes the issuer. Taking the
            // mutex is what closes the lost-wakeup window; it is uncontended in
            // every case but this one.
            if (job.done.fetch_add(1, std::memory_order_release) + 1 == job.num_tasks) {
                std::lock_guard<std::mutex> lock(job.mutex);
                job.finished.notify_all();
            }
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

// The entry point call sites should use. `ThreadPool::instance().parallel_for`
// still works and is what this forwards to; this exists so a call site reads
// `parallel::for_range(n, 1, fn)` rather than stuttering the word twice.
inline void for_range(std::size_t n, std::size_t min_chunk,
                      const std::function<void(std::size_t, std::size_t)>& fn) {
    ThreadPool::instance().parallel_for(n, min_chunk, fn);
}

// The same, said out loud. A call site that knows what its work is for says so
// here; one that does not keeps the overload above and means UserInitiated.
inline void for_range(std::size_t n, std::size_t min_chunk,
                      const std::function<void(std::size_t, std::size_t)>& fn, WorkClass cls) {
    ThreadPool::instance().parallel_for(n, min_chunk, fn, cls);
}

}  // namespace parallel
}  // namespace clay
