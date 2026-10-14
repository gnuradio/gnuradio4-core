#include <boost/ut.hpp>

#include <gnuradio-4.0/thread/thread_pool.hpp>

#include <atomic>
#include <exception>
#include <memory>
#include <print>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <vector>

#if defined(__linux__) && !defined(__EMSCRIPTEN__) && !defined(EMBEDDED)
#include <sys/resource.h>
#endif

namespace {
constexpr std::size_t kLoweredThreadLimit = 32UZ;
constexpr std::size_t kQueuedRuns         = 100UZ;

// The thread limit is read once from RLIMIT_NPROC. Lowering the soft limit around that first read gives this process a
// small pool limit; restoring it at once keeps the kernel from refusing threads below the original limit. The kernel
// checks the limit only when a thread starts, and no thread starts while the lowered limit is in force. The test lowers
// the limit only on Linux without EMBEDDED, where the pool reads RLIMIT_NPROC and counts every thread of the process.
// Elsewhere, or where the soft limit cannot be lowered, threadLimit is zero.
const std::size_t threadLimit = [] {
#if defined(__linux__) && !defined(__EMSCRIPTEN__) && !defined(EMBEDDED)
    rlimit original{};
    if (getrlimit(RLIMIT_NPROC, &original) == 0) {
        rlimit lowered   = original;
        lowered.rlim_cur = static_cast<rlim_t>(kLoweredThreadLimit);
        if (setrlimit(RLIMIT_NPROC, &lowered) == 0) {
            const std::size_t limit = gr::thread_pool::thread::getThreadLimit();
            setrlimit(RLIMIT_NPROC, &original);
            return limit;
        }
    }
#endif
    return 0UZ;
}();

// The kernel counts a thread until the thread has finished exiting, which can be after its join returns. The pool reads
// that count at its limit check.
void waitForThreadCount(std::size_t count) {
    while (gr::thread_pool::getTotalThreadCount() > count) {
        std::this_thread::yield();
    }
}

// Starts threads until the pool can add only `room` more workers; with no room, one more thread would reach threadLimit.
// It starts at most kLoweredThreadLimit threads.
// The count is exact when no joined thread is still counted. Each case starts with the count settled, and the
// destructor waits until the kernel has dropped the fillers from the count.
struct LimitFillers {
    std::atomic<bool>        release{false};
    std::vector<std::thread> threads;

    explicit LimitFillers(std::size_t room = 0UZ) {
        try {
            while (threads.size() < kLoweredThreadLimit && gr::thread_pool::getTotalThreadCount() + 1UZ + room < threadLimit) {
                add();
            }
        } catch (...) {
            joinAll();
            throw;
        }
    }

    ~LimitFillers() { joinAll(); }

    void add() {
        threads.emplace_back([this] { release.wait(false); });
    }

    void joinAll() {
        const std::size_t counted = gr::thread_pool::getTotalThreadCount();
        release                   = true;
        release.notify_all();
        for (std::thread& thread : threads) {
            thread.join();
        }
        waitForThreadCount(counted - threads.size());
    }
};

// Empty when the cases can run. A case runs up to kLoweredThreadLimit threads at once. A per-user or container thread
// limit can refuse them when the user's other threads come near it.
const std::string_view skipReason = []() -> std::string_view {
    if (threadLimit != kLoweredThreadLimit) {
        return "the test lowers the thread limit only on Linux without EMBEDDED, through the soft RLIMIT_NPROC before its first read";
    }
    try {
        LimitFillers fillers;
        fillers.add();
    } catch (const std::exception&) {
        return "the system refuses this process the threads that fill the lowered limit, or the thread count cannot be read";
    }
    return {};
}();

// Runs `body`, then waits until the count is back to its value before `body`. `body` joins every thread it starts.
void runSettled(auto body) {
    const std::size_t settled = gr::thread_pool::getTotalThreadCount();
    body();
    waitForThreadCount(settled);
}

// A worker's thread-local object whose destructor holds the thread's exit until released. The pool joins a worker that
// left before it creates the next one, and that creation stays held after its reservation and before the limit check.
struct HeldThreadExit {
    static inline std::atomic<bool> entered{false};
    static inline std::atomic<bool> release{false};

    ~HeldThreadExit() {
        entered = true;
        entered.notify_all();
        release.wait(false);
    }
};

// The task notifies after the store that ends the wait. It holds the flag through a shared pointer.
void runAndWait(gr::thread_pool::BasicThreadPool& pool) {
    auto ran = std::make_shared<std::atomic<bool>>(false);
    pool.execute([ran] {
        *ran = true;
        ran->notify_all();
    });
    ran->wait(false);
}
} // namespace

const boost::ut::suite<"gr::thread_pool global thread limit"> threadLimitTests = [] {
    using namespace boost::ut;
    using namespace gr::thread_pool;

    const auto limitTest = [](std::string_view name, auto body) {
        if (skipReason.empty()) {
            test(name) = [body] { runSettled(body); };
        } else {
            std::println("skipping '{}': {}", name, skipReason);
            skip / test(name) = body;
        }
    };

    // Each case declares the state that its tasks use before the pool. A task can still be returning from a wait when
    // the case ends, and the pool's destructor joins its workers before that state ends.
    limitTest("ThreadPool: a task submitted at the global thread limit with no idle worker is refused, not queued", [] {
        std::atomic<bool> firstStarted{false};
        std::atomic<bool> releaseFirst{false};
        std::atomic<bool> refusedRan{false};
        BasicThreadPool   pool("LimitTest", TaskType::IO_BOUND, 1U, static_cast<std::uint32_t>(2UZ * kLoweredThreadLimit));
        pool.waitUntilInitialised();

        pool.execute([&] {
            firstStarted = true;
            firstStarted.notify_all();
            releaseFirst.wait(false);
        });
        firstStarted.wait(false);

        {
            LimitFillers fillers;
            expect(throws<std::out_of_range>([&] { pool.execute([&refusedRan] { refusedRan = true; }); })) << "the caller learns that no worker can run the task";
            expect(eq(pool.numThreads(), 1UZ));
            expect(eq(pool.numTasksQueued(), 0UZ)) << "a refused task is not queued";
        }

        releaseFirst = true;
        releaseFirst.notify_all();
        runAndWait(pool);
        expect(!refusedRan.load()) << "the refused task never runs";
    });

    limitTest("ThreadPool: a pool with no worker refuses a task at the global thread limit", [] {
        std::atomic<bool> refusedRan{false};
        BasicThreadPool   pool("LimitNoWorkerTest", TaskType::IO_BOUND, 0U, 8U);
        expect(eq(pool.numThreads(), 0UZ));

        {
            LimitFillers fillers;
            expect(throws<std::out_of_range>([&] { pool.execute([&refusedRan] { refusedRan = true; }); })) << "a pool with no worker cannot run the task";
            expect(eq(pool.numThreads(), 0UZ));
            expect(eq(pool.numTasksQueued(), 0UZ)) << "a refused task is not queued";
        }

        pool.setThreadBounds(1U, 8U); // a minimum of one keeps the added worker past its keep-alive
        runAndWait(pool);
        expect(eq(pool.numThreads(), 1UZ)) << "below the limit the pool adds a worker";
        expect(!refusedRan.load()) << "the refused task never runs";
    });

    limitTest("ThreadPool: a task submitted at the global thread limit behind a task its only worker has not started is refused", [] {
        // At its maximum of one worker the pool queues the first task without waiting for the worker to take it. Raising
        // the maximum then lets the second submission try to grow. The worker can take the first task before that
        // submission. One task queued right after the refusal shows that it had not. The case repeats, up to
        // kQueuedRuns times, until a run shows it.
        const auto refuseBehindFirst = [] {
            std::atomic<bool> firstStarted{false};
            std::atomic<bool> releaseFirst{false};
            std::atomic<bool> refusedRan{false};
            bool              firstQueued = false;
            BasicThreadPool   pool("LimitQueuedTest", TaskType::IO_BOUND, 0U, 1U);

            {
                LimitFillers fillers(1UZ);
                pool.execute([&] {
                    firstStarted = true;
                    firstStarted.notify_all();
                    releaseFirst.wait(false);
                });
                pool.setThreadBounds(1U, 2U);
                expect(throws<std::out_of_range>([&] { pool.execute([&refusedRan] { refusedRan = true; }); })) << "the only worker has the first task. The second needs a worker the pool cannot add";
                firstQueued = pool.numTasksQueued() == 1UZ;
                firstStarted.wait(false);
                expect(eq(pool.numThreads(), 1UZ));
                expect(eq(pool.numTasksQueued(), 0UZ)) << "the refused task is not queued behind the first";
            }

            releaseFirst = true;
            releaseFirst.notify_all();
            runAndWait(pool);
            expect(!refusedRan.load()) << "the refused task never runs";
            return firstQueued;
        };

        bool firstQueued = false;
        for (std::size_t run = 0UZ; run < kQueuedRuns && !firstQueued; ++run) {
            runSettled([&] { firstQueued = refuseBehindFirst(); });
        }
        expect(firstQueued) << "a run submits the second task while the first is still queued";
    });

    limitTest("ThreadPool: a task submitted while the pool's last worker is being created and then refused is refused too", [] {
        std::atomic<bool> dependentRan{false};
        BasicThreadPool   pool("LimitCreatingTest", TaskType::IO_BOUND, 0U, 1U);
        pool.keepAliveDuration = std::chrono::milliseconds(1);
        pool.execute([] { [[maybe_unused]] thread_local HeldThreadExit heldExit; });
        HeldThreadExit::entered.wait(false);
        expect(eq(pool.numThreads(), 0UZ)) << "the worker has left and its exit is held";

        std::atomic<bool> dependentDone{false};
        std::atomic<bool> reservingRefused{false};
        std::thread       reserving([&] {
            try {
                pool.execute([] {});
            } catch (const std::out_of_range&) {
                reservingRefused = true;
            }
            dependentDone.wait(false); // the thread stays counted until the dependent submitter is done
        });
        // the reserving submitter holds the only worker slot and waits to join the worker that left
        while (pool.numThreads() == 0UZ) {
            std::this_thread::yield();
        }

        std::atomic<bool> dependentRefused{false};
        std::thread       dependent([&] {
            try {
                pool.execute([&dependentRan] { dependentRan = true; });
            } catch (const std::out_of_range&) {
                dependentRefused = true;
            }
            dependentDone = true;
            dependentDone.notify_all();
        });
        // the dependent submitter has counted its task and found the pool at its maximum
        while (pool.numTasksQueued() < 2UZ) {
            std::this_thread::yield();
        }

        {
            LimitFillers fillers;
            // one more filler takes the place of the worker that left once the pool joins it
            fillers.add();
            HeldThreadExit::release = true;
            HeldThreadExit::release.notify_all();
            dependent.join();
            reserving.join();

            expect(reservingRefused.load()) << "the reserving submitter learns that its worker was refused";
            expect(dependentRefused.load()) << "the dependent submitter learns that no worker can run its task";
            expect(eq(pool.numThreads(), 0UZ));
            expect(eq(pool.numTasksQueued(), 0UZ)) << "no accepted task waits in a pool without a worker";
        }

        runAndWait(pool);
        expect(!dependentRan.load()) << "the refused task never runs";
    });
};

int main() { /* tests are statically executed */ }
