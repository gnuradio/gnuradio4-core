#include <boost/ut.hpp>

#include <gnuradio-4.0/thread/thread_pool.hpp>

#include <thread>

#if defined(__linux__) && !defined(__EMSCRIPTEN__)
#include <sys/resource.h>
#endif

namespace {
constexpr std::size_t kLoweredThreadLimit = 32UZ;

// The thread limit is read once from RLIMIT_NPROC. Lowering the soft limit around that first read gives this process a
// small pool limit; restoring it at once keeps the kernel from refusing threads below the original limit.
const std::size_t threadLimit = [] {
#if defined(__linux__) && !defined(__EMSCRIPTEN__)
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
    return gr::thread_pool::thread::getThreadLimit();
}();

// Starts threads until the pool can add only `room` more workers; with no room, one more thread would reach threadLimit.
struct LimitFillers {
    std::atomic<bool>        release{false};
    std::vector<std::thread> threads;

    explicit LimitFillers(std::size_t room = 0UZ) {
        while (gr::thread_pool::getTotalThreadCount() + 1UZ + room < threadLimit) {
            threads.emplace_back([this] { release.wait(false); });
        }
    }

    ~LimitFillers() {
        release = true;
        release.notify_all();
        for (std::thread& thread : threads) {
            thread.join();
        }
    }
};

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

void runAndWait(gr::thread_pool::BasicThreadPool& pool) {
    std::atomic<bool> ran{false};
    pool.execute([&ran] {
        ran = true;
        ran.notify_all();
    });
    ran.wait(false);
}
} // namespace

const boost::ut::suite<"gr::thread_pool global thread limit"> threadLimitTests = [] {
    using namespace boost::ut;
    using namespace gr::thread_pool;

    // the cases need a lowered limit, which only Linux provides here
    const auto limitTest = [](std::string_view name, auto body) {
        if (threadLimit == kLoweredThreadLimit) {
            test(name) = body;
        } else {
            skip / test(name) = body;
        }
    };

    limitTest("ThreadPool: a task submitted at the global thread limit with no idle worker is refused, not queued", [] {
        BasicThreadPool pool("LimitTest", TaskType::IO_BOUND, 1U, static_cast<std::uint32_t>(2UZ * kLoweredThreadLimit));
        pool.waitUntilInitialised();

        std::atomic<bool> firstStarted{false};
        std::atomic<bool> releaseFirst{false};
        pool.execute([&] {
            firstStarted = true;
            firstStarted.notify_all();
            releaseFirst.wait(false);
        });
        firstStarted.wait(false);

        std::atomic<bool> refusedRan{false};
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
        BasicThreadPool pool("LimitNoWorkerTest", TaskType::IO_BOUND, 0U, 8U);
        expect(eq(pool.numThreads(), 0UZ));

        std::atomic<bool> refusedRan{false};
        {
            LimitFillers fillers;
            expect(throws<std::out_of_range>([&] { pool.execute([&refusedRan] { refusedRan = true; }); })) << "a pool with no worker cannot run the task";
            expect(eq(pool.numThreads(), 0UZ));
            expect(eq(pool.numTasksQueued(), 0UZ)) << "a refused task is not queued";
        }

        runAndWait(pool);
        expect(eq(pool.numThreads(), 1UZ)) << "below the limit the pool adds a worker";
        expect(!refusedRan.load()) << "the refused task never runs";
    });

    limitTest("ThreadPool: a task submitted at the global thread limit behind a task its only worker has not started is refused", [] {
        // At its maximum of one worker the pool queues the first task without waiting for the worker to take it. Raising
        // the maximum then lets the second submission try to grow while the first task can still be in the queue.
        BasicThreadPool pool("LimitQueuedTest", TaskType::IO_BOUND, 0U, 1U);

        std::atomic<bool> firstStarted{false};
        std::atomic<bool> releaseFirst{false};
        std::atomic<bool> refusedRan{false};
        {
            LimitFillers fillers(1UZ);
            pool.execute([&] {
                firstStarted = true;
                firstStarted.notify_all();
                releaseFirst.wait(false);
            });
            pool.setThreadBounds(1U, 2U);
            expect(throws<std::out_of_range>([&] { pool.execute([&refusedRan] { refusedRan = true; }); })) << "the only worker has the first task. The second needs a worker the pool cannot add";
            firstStarted.wait(false);
            expect(eq(pool.numThreads(), 1UZ));
            expect(eq(pool.numTasksQueued(), 0UZ)) << "the refused task is not queued behind the first";
        }

        releaseFirst = true;
        releaseFirst.notify_all();
        runAndWait(pool);
        expect(!refusedRan.load()) << "the refused task never runs";
    });

    limitTest("ThreadPool: a task submitted while the pool's last worker is being created and then refused is refused too", [] {
        BasicThreadPool pool("LimitCreatingTest", TaskType::IO_BOUND, 0U, 1U);
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
        std::atomic<bool> dependentRan{false};
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
            fillers.threads.emplace_back([&fillers] { fillers.release.wait(false); });
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
