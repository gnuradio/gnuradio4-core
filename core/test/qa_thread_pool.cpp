#include <boost/ut.hpp>

#include <gnuradio-4.0/meta/UnitTestHelper.hpp>
#include <gnuradio-4.0/thread/thread_pool.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <optional>
#include <semaphore>
#include <string>
#include <thread>
#include <vector>

#if not defined(__EMSCRIPTEN__) && not defined(__APPLE__)
namespace {

// Tasks that hold their workers until released. Each task records the name and the affinity mask of its worker. An
// instance declared before the pool outlives the workers that read it.
struct HeldWorkers {
    struct Worker {
        std::string       name;
        std::vector<bool> mask;
    };

    std::mutex               mutex;
    std::condition_variable  changed;
    std::vector<Worker>      workers;
    std::vector<std::string> releasedNames;
    bool                     releasedAll = false;

    // Queues up to nTasks tasks and waits until each queued task holds a worker. Returns the message of the refusal
    // that stops the queueing, or an empty string when the pool takes every task.
    std::string hold(gr::thread_pool::BasicThreadPool& pool, std::size_t nTasks) {
        std::string refusal;
        std::size_t nQueued = 0UZ;
        try {
            for (; nQueued < nTasks; ++nQueued) {
                pool.execute([this] { holdWorker(); });
            }
        } catch (const std::exception& e) {
            refusal = e.what();
        }
        std::unique_lock lock(mutex);
        changed.wait(lock, [this, nQueued] { return workers.size() >= nQueued; });
        return refusal;
    }

    // The mask of the worker with the given name, or an empty mask when no task ran on that worker.
    [[nodiscard]] std::vector<bool> maskOf(std::string_view name) const {
        const auto found = std::ranges::find(workers, name, &Worker::name);
        return found == workers.end() ? std::vector<bool>{} : found->mask;
    }

    // Releases the task held by the worker with the given name.
    void release(std::string name) {
        {
            std::scoped_lock lock(mutex);
            releasedNames.push_back(std::move(name));
        }
        changed.notify_all();
    }

    void releaseAll() {
        {
            std::scoped_lock lock(mutex);
            releasedAll = true;
        }
        changed.notify_all();
    }

private:
    void holdWorker() {
        std::unique_lock  lock(mutex);
        const std::string name = gr::thread_pool::thread::getThreadName();
        workers.push_back({.name = name, .mask = gr::thread_pool::thread::getThreadAffinity()});
        changed.notify_all();
        changed.wait(lock, [this, &name] { return releasedAll || std::ranges::find(releasedNames, name) != releasedNames.end(); });
    }
};

// A worker leaves at its keep-alive without a signal. The wait polls the pool's thread count for five seconds at most.
bool waitForNumThreads(const gr::thread_pool::BasicThreadPool& pool, std::size_t nThreads) {
    for (std::size_t i = 0UZ; i < 5000UZ && pool.numThreads() != nThreads; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return pool.numThreads() == nThreads;
}

// Two CPUs of the calling thread. The pool's mask holds both, and each stripe holds one.
struct TwoCpuMask {
    std::vector<bool>                pool;
    std::array<std::vector<bool>, 2> stripes;
};

// Returns no mask when the calling thread may run on fewer than nCallerCpus CPUs.
std::optional<TwoCpuMask> twoCpuMask(std::size_t nCallerCpus) {
    const std::vector<bool>  callerMask = gr::thread_pool::thread::getThreadAffinity();
    std::vector<std::size_t> callerCpus;
    for (std::size_t cpu = 0UZ; cpu < callerMask.size(); ++cpu) {
        if (callerMask[cpu]) {
            callerCpus.push_back(cpu);
        }
    }
    if (callerCpus.size() < nCallerCpus) {
        return std::nullopt;
    }
    TwoCpuMask mask{.pool = std::vector<bool>(callerMask.size(), false), .stripes = {}};
    mask.stripes.fill(mask.pool);
    for (std::size_t i = 0UZ; i < mask.stripes.size(); ++i) {
        mask.pool[callerCpus[i]]       = true;
        mask.stripes[i][callerCpus[i]] = true;
    }
    return mask;
}

std::string bits(const std::vector<bool>& mask) {
    std::string text;
    for (const bool cpuSet : mask) {
        text.push_back(cpuSet ? '1' : '0');
    }
    return text;
}

} // namespace
#endif

const boost::ut::suite<"gr::thread_pool GR4 default"> defaultThreadPool = [] {
    using namespace boost::ut;

    "Basic ThreadPool tests"_test = [] {
        expect(nothrow([] { gr::thread_pool::BasicThreadPool("test", gr::thread_pool::IO_BOUND, 4UL); }));
        expect(nothrow([] { gr::thread_pool::BasicThreadPool("test2", gr::thread_pool::CPU_BOUND, 4UL); }));

        std::atomic<int>                 enqueueCount{0};
        std::atomic<int>                 executeCount{0};
        gr::thread_pool::BasicThreadPool pool("TestPool", gr::thread_pool::IO_BOUND, 1, 2);
        expect(nothrow([&] { pool.sleepDuration = std::chrono::milliseconds(1); }));
        expect(nothrow([&] { pool.keepAliveDuration = std::chrono::seconds(10); }));
        pool.waitUntilInitialised();
        expect(that % pool.isInitialised());
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        expect(pool.poolName() == "TestPool");
        expect(pool.minThreads() == 1U);
        expect(pool.maxThreads() == 2U);
        expect(pool.numThreads() == 1U);
        expect(pool.numTasksRunning() == 0U);
        expect(pool.numTasksQueued() == 0U);
        expect(pool.numTasksRecycled() == 0U);
        pool.execute([&enqueueCount] {
            ++enqueueCount;
            enqueueCount.notify_all();
        });
        enqueueCount.wait(0);
        expect(pool.numThreads() == 1U);
        pool.execute([&executeCount] {
            ++executeCount;
            executeCount.notify_all();
        });
        executeCount.wait(0);
        expect(pool.numThreads() >= 1U);
        expect(enqueueCount.load() == 1);
        expect(executeCount.load() == 1);

        auto ret = pool.execute([] { return 42; });
        expect(ret.get() == 42);

        auto taskName = pool.execute<"taskName", 0, -1>([] { return gr::thread_pool::thread::getThreadName(); });
#if defined(__EMSCRIPTEN__) || defined(__APPLE__)
        expect(taskName.get() == "unknown thread name"_b);
#else
        expect(taskName.get() == "taskName"_b);
#endif

        expect(nothrow([&] { pool.setAffinityMask(pool.getAffinityMask()); }));
        expect(nothrow([&] { pool.setThreadSchedulingPolicy(pool.getSchedulingPolicy(), pool.getSchedulingPriority()); }));
    };

    "contention tests"_test = [] {
        std::atomic<std::size_t>         counter{0UZ};
        gr::thread_pool::BasicThreadPool pool("contention", gr::thread_pool::IO_BOUND, 1, 4);
        pool.waitUntilInitialised();
        expect(that % pool.isInitialised());
        expect(pool.numThreads() == 1U);
        pool.execute([&counter] {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            std::atomic_fetch_add(&counter, 1UZ);
            counter.notify_all();
        });
        expect(pool.numThreads() == 1U);
        pool.execute([&counter] {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            std::atomic_fetch_add(&counter, 1UZ);
            counter.notify_all();
        });
        expect(pool.numThreads() >= 1UZ);
        counter.wait(0UZ);
        counter.wait(1UZ);
        expect(counter.load() == 2UZ);
    };

    "ThreadPool: Thread count tests"_test = [] {
        struct bounds_def {
            std::uint32_t min, max;
        };
        std::array<bounds_def, 5> bounds{bounds_def{1, 1}, bounds_def{1, 4}, bounds_def{2, 2}, bounds_def{2, 8}, bounds_def{4, 8}};

        for (const auto [minThreads, maxThreads] : bounds) {
            for (const auto taskCount : {2UZ, 8UZ, 32UZ}) {
                std::print("## Test with min={} and max={} and taskCount={}\n", minThreads, maxThreads, taskCount);
                std::atomic<std::size_t> counter{0UZ};

                // Pool with min and max thread count
                gr::thread_pool::BasicThreadPool pool("count_test", gr::thread_pool::IO_BOUND, minThreads, maxThreads);
                pool.keepAliveDuration = std::chrono::milliseconds(10); // default is 10 seconds, reducing for testing
                pool.waitUntilInitialised();

                for (std::size_t i = 0UZ; i < taskCount; ++i) {
                    pool.execute([&counter] {
                        std::this_thread::sleep_for(std::chrono::milliseconds(10UZ));
                        std::atomic_fetch_add(&counter, 1UZ);
                        counter.notify_all();
                    });
                }
                expect(that % pool.numThreads() >= minThreads);
                // expect(that % pool.numThreads() == std::min(std::uint32_t(taskCount), maxThreads));

                for (std::size_t i = 0UZ; i < taskCount; ++i) {
                    counter.wait(i);
                    expect(that % pool.numThreads() >= minThreads);
                    expect(that % pool.numThreads() <= maxThreads);
                }

                // We should have gotten back to minimum
                std::this_thread::sleep_for(std::chrono::milliseconds(100UZ));
                expect(that % pool.numThreads() == minThreads);
                expect(that % counter.load() == taskCount);
            }
        }
    };

#if not defined(__EMSCRIPTEN__) && not defined(__APPLE__)
    "ThreadPool: an affinity mask pins each worker to its stripe and leaves the calling thread's mask"_test = [] {
        using namespace gr::thread_pool;

        // The pool's mask leaves out at least one CPU of the calling thread. A pool of two workers has two stripes of
        // one CPU each.
        const std::vector<bool>         callerMask = thread::getThreadAffinity();
        const std::optional<TwoCpuMask> mask       = twoCpuMask(3UZ);
        if (!mask) {
            boost::ut::log << "skipped: the calling thread may run on fewer than three CPUs";
            return;
        }

        for (const TaskType taskType : {TaskType::IO_BOUND, TaskType::CPU_BOUND}) {
            const std::string_view poolKind = taskType == TaskType::IO_BOUND ? "IO-bound" : "CPU-bound";
            // An IO-bound worker runs on the whole mask and a CPU-bound worker on its stripe. The third worker shares the first stripe.
            const auto expectedMask = [&](std::size_t worker) { return taskType == TaskType::IO_BOUND ? mask->pool : mask->stripes[worker % mask->stripes.size()]; };

            HeldWorkers     held;
            BasicThreadPool pool("AffinityTest", taskType, 2U, 3U);
            pool.waitUntilInitialised();
            pool.setAffinityMask(mask->pool);
            // Three tasks for two idle workers make the pool start its third worker.
            const std::string       refusal         = held.hold(pool, 3UZ);
            const std::vector<bool> callerMaskAfter = thread::getThreadAffinity();
            held.releaseAll();

            expect(refusal.empty()) << std::format("{} pool: the third worker was refused: {}", poolKind, refusal);
            expect(callerMaskAfter == callerMask) << std::format("{} pool: the calling thread's mask {} became {}", poolKind, bits(callerMask), bits(callerMaskAfter));
            for (std::size_t worker = 0UZ; worker < 3UZ; ++worker) {
                const std::vector<bool> workerMask = held.maskOf(std::format("AffinityTest#{}", worker));
                expect(workerMask == expectedMask(worker)) << std::format("{} pool: worker {} runs on '{}', expected '{}'", poolKind, worker, bits(workerMask), bits(expectedMask(worker)));
            }
        }
    };

    "ThreadPool: a CPU-bound pool with more workers than CPUs in its mask pins every worker to one CPU of the mask"_test = [] {
        using namespace gr::thread_pool;

        const std::optional<TwoCpuMask> mask = twoCpuMask(2UZ);
        if (!mask) {
            boost::ut::log << "skipped: the calling thread may run on fewer than two CPUs";
            return;
        }

        // Three workers share two stripes of one CPU each. A fourth task makes the pool start a fourth worker, which
        // takes the second stripe.
        HeldWorkers     held;
        BasicThreadPool pool("FewCpusTest", TaskType::CPU_BOUND, 3U, 4U);
        pool.waitUntilInitialised();
        std::string maskFailure;
        try {
            pool.setAffinityMask(mask->pool);
        } catch (const std::exception& e) {
            maskFailure = e.what();
        }
        const std::string refusal = held.hold(pool, 4UZ);
        held.releaseAll();

        expect(maskFailure.empty()) << std::format("setAffinityMask() failed: {}", maskFailure);
        expect(refusal.empty()) << std::format("the fourth worker was refused: {}", refusal);
        for (std::size_t worker = 0UZ; worker < 4UZ; ++worker) {
            const std::vector<bool>& expectedMask = mask->stripes[worker % mask->stripes.size()];
            const std::vector<bool>  workerMask   = held.maskOf(std::format("FewCpusTest#{}", worker));
            expect(workerMask == expectedMask) << std::format("worker {} runs on '{}', expected '{}'", worker, bits(workerMask), bits(expectedMask));
        }
    };

    "ThreadPool: a worker started after another left takes the lowest index no worker holds"_test = [] {
        using namespace gr::thread_pool;

        const std::optional<TwoCpuMask> mask = twoCpuMask(2UZ);
        if (!mask) {
            boost::ut::log << "skipped: the calling thread may run on fewer than two CPUs";
            return;
        }

        // Workers 0 and 2 run on the first stripe and worker 1 on the second. Worker 1 leaves at its keep-alive. The
        // worker started in its place takes index 1. It carries that index in its name and runs on the second stripe.
        HeldWorkers firstRound;
        HeldWorkers secondRound;
        // the pool starts its workers at the tasks, after the keep-alive is set
        BasicThreadPool pool("RegrowTest", TaskType::CPU_BOUND, 0U, 3U);
        pool.keepAliveDuration = std::chrono::milliseconds(10);
        pool.setThreadBounds(2U, 3U);
        pool.setAffinityMask(mask->pool);

        std::string refusal = firstRound.hold(pool, 3UZ);
        firstRound.release("RegrowTest#1");
        const bool workerLeft = waitForNumThreads(pool, 2UZ);
        firstRound.releaseAll();
        if (refusal.empty()) {
            refusal = secondRound.hold(pool, 3UZ);
        }
        secondRound.releaseAll();

        expect(refusal.empty()) << std::format("a worker was refused: {}", refusal);
        expect(workerLeft) << "worker 1 did not leave at its keep-alive";
        std::vector<std::string> names;
        for (const HeldWorkers::Worker& worker : secondRound.workers) {
            names.push_back(worker.name);
        }
        std::ranges::sort(names);
        std::string nameList;
        for (const std::string& name : names) {
            nameList += nameList.empty() ? name : ", " + name;
        }
        expect(names == std::vector<std::string>{"RegrowTest#0", "RegrowTest#1", "RegrowTest#2"}) << std::format("the workers are named {}", nameList);
        for (std::size_t worker = 0UZ; worker < 3UZ; ++worker) {
            const std::vector<bool>& expectedMask = mask->stripes[worker % mask->stripes.size()];
            const std::vector<bool>  workerMask   = secondRound.maskOf(std::format("RegrowTest#{}", worker));
            expect(workerMask == expectedMask) << std::format("worker {} runs on '{}', expected '{}'", worker, bits(workerMask), bits(expectedMask));
        }
    };
#endif

    "ThreadPool: CPU affinity rejection"_test = [] {
        using namespace gr::thread_pool;

        BasicThreadPool pool("AffinityReject", TaskType::CPU_BOUND, 1U, 2U);
        pool.waitUntilInitialised();

        // Set affinity mask to enable only CPU 0
        pool.setAffinityMask({true, false, false});

        expect(throws<std::invalid_argument>([&] { pool.execute<"bad_affinity", 0, 1>([] { std::println("should not run"); }); }));
    };

    "ThreadPool: exception propagation"_test = [] {
        using namespace gr::thread_pool;

        BasicThreadPool pool("ExceptionTest", TaskType::IO_BOUND, 1U, 1U);
        pool.waitUntilInitialised();

        auto fut = pool.execute([]() -> int { throw std::runtime_error("expected failure"); });

        expect(throws<std::runtime_error>([&] { (void)fut.get(); }));
    };

    "ThreadPool: a throwing void task must not kill the worker"_test = [] {
        using namespace gr::thread_pool;

        BasicThreadPool pool("ThrowingTaskTest", TaskType::IO_BOUND, 1U, 1U);
        pool.waitUntilInitialised();

        const std::size_t nFailedBefore = pool.numTasksFailed();
        pool.execute([] { throw std::runtime_error("expected failure"); });
        for (std::size_t i = 0UZ; i < 2000UZ && pool.numTasksFailed() == nFailedBefore; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        expect(eq(pool.numTasksFailed(), nFailedBefore + 1UZ)) << "the escaping exception should be caught and counted";

        std::atomic<bool> ranAfterThrow{false};
        pool.execute([&ranAfterThrow] {
            ranAfterThrow = true;
            ranAfterThrow.notify_all();
        });
        ranAfterThrow.wait(false);
        expect(ranAfterThrow.load()) << "the worker must keep serving tasks after one threw";
    };

    "ThreadPool: a task submitted to an idle worker never waits for the keep-alive"_test = [] {
        using namespace gr::thread_pool;
        using Clock = std::chrono::steady_clock;

        // the pool starts its only worker at the first task, after the keep-alive is set
        BasicThreadPool pool("IdleWakeUpTest", TaskType::IO_BOUND, 0U, 1U);
        pool.keepAliveDuration = std::chrono::seconds(1);

        // the delays sweep the moment the idle worker blocks on the condition variable
        constexpr std::size_t kSubmissions = 50'000UZ;
        std::binary_semaphore started{0};
        Clock::duration       longestWait{};
        bool                  allStarted = true;
        for (std::size_t i = 0UZ; i < kSubmissions; ++i) {
            const Clock::time_point submitAfter = Clock::now() + std::chrono::nanoseconds((i * 37UZ) % 20'000UZ);
            while (Clock::now() < submitAfter) {
            }
            const Clock::time_point submitted = Clock::now();
            pool.execute([&started] { started.release(); });
            // The wait is bounded. A task that never starts fails the case.
            if (!started.try_acquire_for(2 * pool.keepAliveDuration)) {
                allStarted = false;
                break;
            }
            longestWait = std::max(longestWait, Clock::now() - submitted);
        }
        const auto longestWaitMs = std::chrono::duration_cast<std::chrono::milliseconds>(longestWait).count();
        expect(allStarted) << "a task did not start";
        expect(lt(longestWaitMs, (pool.keepAliveDuration / 2).count())) << "a task waited for the worker's keep-alive timeout";
    };

    "ThreadPool: tasks submitted at once from several threads each start on a worker of their own"_test = [] {
        using namespace gr::thread_pool;
        using Clock = std::chrono::steady_clock;

        // Each task waits until every task of its round has started. A task queued behind a busy worker while the pool
        // could still add one does not start within the round.
        constexpr std::size_t kRounds = 500UZ;
        for (const std::size_t nSubmitters : {4UZ, 8UZ}) {
            std::size_t firstStuckRound = kRounds;
            for (std::size_t round = 0UZ; round < kRounds && firstStuckRound == kRounds; ++round) {
                std::atomic<std::size_t> nStarted{0UZ};
                std::atomic<bool>        go{false};
                std::atomic<bool>        stuck{false};
                BasicThreadPool          pool("ConcurrentSubmitTest", TaskType::IO_BOUND, 1U, static_cast<std::uint32_t>(nSubmitters));
                pool.waitUntilInitialised();
                std::vector<std::thread> submitters;
                for (std::size_t i = 0UZ; i < nSubmitters; ++i) {
                    submitters.emplace_back([&] {
                        // the submitters spin on the flag and reach execute() together
                        while (!go.load(std::memory_order_acquire)) {
                        }
                        pool.execute([&] {
                            nStarted.fetch_add(1UZ);
                            const Clock::time_point deadline = Clock::now() + std::chrono::seconds(5);
                            while (nStarted.load() < nSubmitters) {
                                if (Clock::now() > deadline) {
                                    stuck = true;
                                    break;
                                }
                                std::this_thread::yield();
                            }
                        });
                    });
                }
                go.store(true, std::memory_order_release);
                for (std::thread& submitter : submitters) {
                    submitter.join();
                }
                const Clock::time_point deadline = Clock::now() + std::chrono::seconds(10);
                while (nStarted.load() < nSubmitters && !stuck.load() && Clock::now() < deadline) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                }
                if (stuck.load() || nStarted.load() < nSubmitters) {
                    firstStuckRound = round;
                }
            }
            expect(eq(firstStuckRound, kRounds)) << std::format("with {} submitters a task did not start in round {}", nSubmitters, firstStuckRound);
        }
    };

    "ThreadPool: recycled task count increases"_test = [] {
        using namespace gr::thread_pool;

        BasicThreadPool pool("RecycleTest", TaskType::IO_BOUND, 1U, 1U);
        pool.waitUntilInitialised();

        const auto before = pool.numTasksRecycled();

        for (std::size_t i = 0; i < 5; ++i) {
            std::atomic<bool> flag{false};
            pool.execute([&] {
                flag = true;
                flag.notify_all();
            });
            flag.wait(false);
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(10)); // let worker recycle

        expect(gt(pool.numTasksRecycled(), before)); // conservatively ≥
    };

    "ThreadPool: setThreadBounds valid and invalid cases"_test = [] {
        using namespace gr::thread_pool;

        BasicThreadPool pool("BoundsTest", TaskType::CPU_BOUND, 2U, 4U);
        pool.waitUntilInitialised();

        expect(nothrow([&] { pool.setThreadBounds(1U, 8U); }));
        expect(pool.minThreads() == 1U);
        expect(pool.maxThreads() == 8U);

        expect(throws<std::invalid_argument>([&] { pool.setThreadBounds(0U, 8U); }));
        expect(throws<std::invalid_argument>([&] { pool.setThreadBounds(2U, 0U); }));
        expect(throws<std::invalid_argument>([&] { pool.setThreadBounds(5U, 4U); }));

        expect(nothrow([&] { pool.setThreadBounds(3U, 3U); }));
        expect(pool.minThreads() == 3U);
        expect(pool.maxThreads() == 3U);
    };

    "ThreadPool: a task submitted as the idle worker's keep-alive expires still runs"_test = [] {
        using namespace gr::thread_pool;
        using Clock = std::chrono::steady_clock;

        // with no minimum, the pool's only worker leaves at its keep-alive and the next task starts a new one
        BasicThreadPool pool("KeepAliveExitTest", TaskType::IO_BOUND, 0U, 1U);
        pool.keepAliveDuration = std::chrono::milliseconds(1);

        // the delays after each task sweep the end of the worker's keep-alive
        constexpr std::size_t kSubmissions = 2'000UZ;
        bool                  allStarted   = true;
        std::size_t           maxHeld      = 0UZ;
        for (std::size_t i = 0UZ; i < kSubmissions; ++i) {
            auto started = std::make_shared<std::promise<void>>();
            auto future  = started->get_future();
            pool.execute([started] { started->set_value(); });
            if (future.wait_for(std::chrono::seconds(2)) != std::future_status::ready) {
                allStarted = false;
                break;
            }
            // a worker that left and is not yet joined still holds its thread
            maxHeld = std::max(maxHeld, pool.numThreadsHeld());

            const Clock::time_point submitAfter = Clock::now() + pool.keepAliveDuration - std::chrono::microseconds(30) + std::chrono::nanoseconds((i * 61UZ) % 120'000UZ);
            while (Clock::now() < submitAfter) {
            }
        }
        expect(allStarted) << "a task queued as the worker left was never started";
        expect(le(maxHeld, static_cast<std::size_t>(pool.maxThreads()))) << "the pool held more threads than its maximum";
        expect(eq(pool.numTasksQueued(), 0UZ));
    };
};

const boost::ut::suite<"gr::thread_pool Manager"> ThreadPoolManager = [] {
    using namespace boost::ut;
    using namespace gr::thread_pool;

    "Manager: default pools registration and retrieval"_test = [] {
        auto cpu = Manager::defaultCpuPool();
        auto io  = Manager::defaultIoPool();

        expect(cpu->type() == TaskType::CPU_BOUND);
        expect(io->type() == TaskType::IO_BOUND);
        expect(cpu->device() == "CPU");
        expect(io->device() == "CPU");
        auto [min, max] = cpu->threadBounds();
        expect(eq(min, cpu->minThreads()));
        expect(eq(max, cpu->maxThreads()));

        std::atomic<std::size_t> flag{0UZ};
        cpu->execute([&] {
            flag = 1UZ;
            flag.notify_all();
        });
        flag.wait(0UZ);

        expect(cpu->numThreads() >= 1U) << "needed to execute one task to spawn one thread";
        expect(cpu->isShutdown() == false);
    };

    "Manager: custom pool registration and execution"_test = [] {
        auto& manager = Manager::instance();

        auto custom = std::make_shared<ThreadPoolWrapper>(std::make_unique<BasicThreadPool>("MyPool", TaskType::IO_BOUND, 1, 2), "VirtualDevice");

        expect(nothrow([&] { manager.registerPool("my_pool", std::move(custom)); }));

        auto pool = manager.get("my_pool");
        expect(pool->name() == "MyPool");
        expect(pool->device() == "VirtualDevice");
        expect(pool->type() == TaskType::IO_BOUND);

        std::atomic<std::size_t> flag{0UZ};
        pool->execute([&] {
            flag = 1UZ;
            flag.notify_all();
        });
        flag.wait(0UZ);
        expect(flag.load() == 1UZ);
    };

    "Manager: duplicate registration fails"_test = [] {
        auto dup = std::make_shared<ThreadPoolWrapper>(std::make_unique<BasicThreadPool>("DupPool", TaskType::CPU_BOUND, 1U, 2U), "CPU");
        expect(throws<std::invalid_argument>([&] { Manager::instance().registerPool("default_cpu", std::move(dup)); }));
    };

    "Manager: unknown pool throws"_test = [] { expect(throws<std::out_of_range>([] { (void)Manager::instance().get("not_existing_pool"); })); };

    "Manager: replacePool allows update of registered pool"_test = [] {
        auto& manager = Manager::instance();

        auto updated = std::make_shared<ThreadPoolWrapper>(std::make_unique<BasicThreadPool>("updated_pool", TaskType::CPU_BOUND, 1U, 1U), "CPU-Updated");
        updated->setThreadBounds(1U, 4U);

        manager.replacePool("default_cpu", std::move(updated));

        auto pool = Manager::defaultCpuPool();
        expect(pool->device() == "CPU-Updated");
        expect(pool->threadBounds().second == 4UZ);

        std::atomic<std::size_t> taskRan{0UZ};
        pool->execute([&] {
            taskRan = 1UZ;
            taskRan.notify_all();
        });
        taskRan.wait(0UZ);
        expect(taskRan.load() == 1UZ);
    };
};

#ifndef GR_MAX_WASM_THREAD_COUNT
#define GR_MAX_WASM_THREAD_COUNT 60 // fallback
#endif

const boost::ut::suite<"gr::thread_pool Manager WASM"> _wasm = [] {
    using namespace boost::ut;
    using namespace gr::thread_pool;

    "getTotalThreadCount"_test = [] {
        const std::size_t count = gr::thread_pool::getTotalThreadCount();
        expect(gt(count, 0UZ));
        expect(le(count, gr::thread_pool::thread::getThreadLimit()));
    };

    "global thread counter tracking thread creation"_test = [] {
        // Wait for the process-wide thread count to stabilise — threads from earlier
        // test suites (with short keepAliveDuration) may still be winding down at the OS level.
        std::size_t before = gr::thread_pool::getTotalThreadCount();
        for (int stableChecks = 0, i = 0; i < 40 && stableChecks < 3; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(25));
            if (const auto now = gr::thread_pool::getTotalThreadCount(); now == before) {
                ++stableChecks;
            } else {
                before       = now;
                stableChecks = 0;
            }
        }
        {
            BasicThreadPool temp("test_pool", CPU_BOUND, 2, 2);
            temp.waitUntilInitialised();
            expect(eq(temp.numThreads(), 2UZ)) << "pool should have exactly 2 threads";
            const std::size_t during = gr::thread_pool::getTotalThreadCount();
            expect(ge(during, before + 2));
        }
        // Pool destructor joins all threads; wait briefly for OS bookkeeping.
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        const std::size_t after = gr::thread_pool::getTotalThreadCount();
        expect(eq(after, before)) << "Expected cleanup after pool destruction";
    };

    "GR_MAX_WASM_THREAD_COUNT compile-time constant"_test = [] {
#ifdef __EMSCRIPTEN__
#ifdef GR_MAX_WASM_THREAD_COUNT
        expect(eq(static_cast<std::size_t>(GR_MAX_WASM_THREAD_COUNT), gr::thread_pool::thread::getThreadLimit()));
#else
        expect(false) << "GR_MAX_WASM_THREAD_COUNT not defined";
#endif
#else
        expect(gt(gr::thread_pool::thread::getThreadLimit(), 1UZ));
#endif
    };

    "Manager: WASM exhausting available threads"_test = [] {
        Manager& manager = Manager::instance();
#ifdef __EMSCRIPTEN__
        const std::size_t poolMaxThreads = gr::thread_pool::thread::getThreadLimit();
        const auto        taskSleep      = std::chrono::seconds(20);
#else
        const std::size_t poolMaxThreads = std::min<std::size_t>(50UZ, gr::thread_pool::thread::getThreadLimit());
        const auto        taskSleep      = std::chrono::seconds(2);
#endif
        // replace IO pool to unlimited upper bound
        manager.replacePool(std::string(kDefaultIoPoolId), std::make_shared<ThreadPoolWrapper>(std::make_unique<BasicThreadPool>(kDefaultIoPoolId, TaskType::IO_BOUND, 1U, poolMaxThreads), "CPU"));
        std::shared_ptr<TaskExecutor> pool = manager.get(gr::thread_pool::kDefaultIoPoolId);

        std::println("HW threads = {} - max wasm threads: {} actual: {} - pool max size: {}", //
            std::thread::hardware_concurrency(), gr::thread_pool::thread::getThreadLimit(), gr::thread_pool::getTotalThreadCount(), pool->maxThreads());
        std::atomic<std::size_t> unexpectedExceptions{0UZ};
        std::atomic<std::size_t> expectedExceptions{0UZ};
        for (std::size_t i = 0UZ; i < poolMaxThreads + 10UZ; ++i) {
            if (i >= (poolMaxThreads - 10UZ)) {
                std::println("start thread {}", i);
            }
            try {
                pool->execute([taskSleep] {
                    std::this_thread::sleep_for(taskSleep); // purposeful sleep
                });
            } catch (std::exception& e) {
                std::println("exception thrown: {} for {} threads", e, gr::thread_pool::getTotalThreadCount());
                expectedExceptions.fetch_add(1UZ, std::memory_order_relaxed);
            } catch (...) {
                std::println("unknown exception thrown for {} threads", gr::thread_pool::getTotalThreadCount());
                unexpectedExceptions.fetch_add(1UZ, std::memory_order_relaxed);
            }
            if ((expectedExceptions.load() + unexpectedExceptions.load()) >= 10UZ) {
                break;
            }
        }
        std::println("number of exceptions thrown: {} unexpeced: {}", expectedExceptions.load(), unexpectedExceptions.load());
#ifdef __EMSCRIPTEN__
        expect(gt(expectedExceptions.load(), 0UZ)) << fatal << "creating more threads than kThreadLimit should throw with expected exception";
#endif
        expect(eq(unexpectedExceptions.load(), 0UZ)) << fatal << "caught unexpected exception";
    };

    "computeDefaultThreadSplit respects invariants under edge conditions"_test = [] {
        using namespace gr::thread_pool::detail;

        const auto validate_split = [](std::size_t threadLimit, std::size_t reserve, std::source_location loc = std::source_location::current()) {
            const auto s      = computeDefaultThreadSplit(threadLimit, reserve);
            const auto usable = (threadLimit > reserve) ? threadLimit - reserve : 1UZ;
            const auto actual = s.cpuThreadsMax + s.ioThreadsMax;

            expect(le(actual, std::max(2UZ, usable)), loc) << std::format("limit={}, reserve={} -> cpu+io={} must not exceed usable={}", threadLimit, reserve, actual, usable);
            expect(le(s.cpuThreadsMax, usable), loc) << std::format("limit={}, reserve={} -> CPU threads {} must be ≤ usable {}", threadLimit, reserve, s.cpuThreadsMax, usable);
            expect(le(s.ioThreadsMax, usable), loc) << std::format("limit={}, reserve={} -> IO threads {} must be ≤ usable {}", threadLimit, reserve, s.ioThreadsMax, usable);

#if defined(__EMSCRIPTEN__)
            expect(ge(s.cpuThreadsMin, 0UZ), loc) << std::format("WASM: limit={}, reserve={} -> At least 1 CPU thread expected (got {})", threadLimit, reserve, s.cpuThreadsMin);
            expect(ge(s.ioThreadsMin, 0UZ), loc) << std::format("WASM: limit={}, reserve={} -> At least 1 CPU thread expected (got {})", threadLimit, reserve, s.cpuThreadsMin);
#else
            expect(ge(s.cpuThreadsMin, 1UZ), loc) << std::format("limit={}, reserve={} -> At least 1 CPU thread expected (got {})", threadLimit, reserve, s.cpuThreadsMin);
            expect(ge(s.ioThreadsMin, 1UZ), loc) << std::format("limit={}, reserve={} -> At least 1 CPU thread expected (got {})", threadLimit, reserve, s.cpuThreadsMin);
#endif
            expect(ge(s.ioThreadsMax, 1UZ), loc) << std::format("limit={}, reserve={} -> At least 1 IO thread expected (got {})", threadLimit, reserve, s.ioThreadsMax);

            expect(eq(s.threadReserve, reserve), loc) << std::format("limit={}, reserve={} -> Expected thread reserve {}, got {}", threadLimit, reserve, reserve, s.threadReserve);
        };

        validate_split(3UZ, 4UZ);     // threadLimit < reserve → usable = 1
        validate_split(4UZ, 4UZ);     // usable = 1
        validate_split(8UZ, 4UZ);     // small WASM/native config
        validate_split(64UZ, 4UZ);    // typical system config
        validate_split(50000UZ, 4UZ); // upper-bound stress test
    };

    "Manager: respects thread limit and budget"_test = [] {
        using namespace gr::thread_pool;

        Manager& manager = Manager::instance();
        auto     cpu     = manager.get(kDefaultCpuPoolId);
        auto     io      = manager.get(kDefaultIoPoolId);

        const std::size_t totalUsed = cpu->numThreads() + io->numThreads();
        const std::size_t threadCap = thread::getThreadLimit();

        std::println("CPU threads: {}, IO threads: {}, total: {}, cap: {}", cpu->numThreads(), io->numThreads(), totalUsed, threadCap);

        expect(le(totalUsed, threadCap - 1)) << "Manager over-allocated threads";
        expect(gt(io->numThreads(), cpu->numThreads())) << "IO thread count should exceed CPU";
    };
};

int main() { /* tests are statically executed */ }
