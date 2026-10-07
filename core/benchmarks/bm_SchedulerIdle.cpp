#include <benchmark.hpp>
#include <boost/ut.hpp>

#include <sys/resource.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <format>
#include <memory>
#include <optional>
#include <print>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <vector>

#include <gnuradio-4.0/Graph.hpp>
#include <gnuradio-4.0/Message.hpp>
#include <gnuradio-4.0/Scheduler.hpp>
#include <gnuradio-4.0/thread/thread_affinity.hpp>
#include <gnuradio-4.0/thread/thread_pool.hpp>

/**
 * @brief What an idle graph, a loaded chain and a late sample cost under each execution policy of `Simple`.
 *
 * Five arms: the four policies, and multiThreadedBlocking with pool_park_count 0. The workers of that arm park at the
 * first pass at the idle back-off's longest sleep. The default arm spends five more passes at that sleep first.
 *
 *   - throughput: a source, a chain of 4 or 16 processOne blocks and a sink, in samples per second. Each arm repeats
 *     its run kLoadedRuns times after one discarded run.
 *   - idle: a source that never publishes and never ends the stream, into a sink. The figure is the process CPU time
 *     from getrusage() over the wall time of the window, in cores. The row "no scheduler" measures the same window
 *     with no graph running, which is the floor of the process. The idle rows also give the value of each round.
 *   - wake: a source whose worker waits inside work() until the sink's worker is idle, then publishes one chunk. The
 *     latency is the time from the publication to the sink's receipt. A multi-threaded policy runs the source and the
 *     sink on two workers. Under multiThreaded the sink's worker sleeps in the idle back-off. Under
 *     multiThreadedBlocking it is parked, and the publication's progress notify wakes it. A single-threaded policy
 *     calls the sink in the pass that published the chunk.
 *   - poll delay: a source that polls the clock in work() and publishes one chunk at its first call after a due time.
 *     Nothing advances the progress sequence at the due time. The delay is the time from the due time to the
 *     publication. A worker parked on the progress sequence calls the source again when its park times out, after up
 *     to timeout_ms.
 *   - message reply: a settings request that the caller's thread sends into msgIn of a run whose source never
 *     publishes. Where the policy parks idle workers, the request goes out once every worker of the run waits on the
 *     progress sequence. The latency is the time from the send to the reply on msgOut.
 *
 * The idle, wake, poll-delay and message arms run interleaved, the order rotated each round, and the first round is
 * discarded. Their rows give the lowest value, the mean and the highest.
 *
 * The run thread of a single-threaded policy is pinned to kSingleCpu. A multi-threaded policy runs on a pool of
 * kMultiCpus.size() threads, and its run thread is pinned to kMultiCpus. Start the process with the same CPU set, for
 * example `taskset -c 2,4,6,8`. The pool threads and the watchdog then stay on those CPUs.
 */

namespace bm_idle {

using Clock = std::chrono::steady_clock;
using gr::scheduler::ExecutionPolicy;

inline constexpr std::array<std::size_t, 4UZ> kMultiCpus{2UZ, 4UZ, 6UZ, 8UZ};
inline constexpr std::array<std::size_t, 1UZ> kSingleCpu{2UZ};
inline constexpr std::string_view             kPoolName = "bm_scheduler_idle_cpu";

inline constexpr auto        kIdleSettle    = std::chrono::milliseconds(500);
inline constexpr auto        kIdleWindow    = std::chrono::seconds(3);
inline constexpr auto        kWakePause     = std::chrono::milliseconds(250);
inline constexpr std::size_t kWakeChunk     = 4096UZ;
inline constexpr std::size_t kLoadedSamples = 20'000'000UZ;
inline constexpr std::size_t kLoadedRuns    = 5UZ;
inline constexpr std::size_t kIdleRounds    = 3UZ;
inline constexpr std::size_t kWakeRounds    = 10UZ;
inline constexpr float       kGain          = 1.0000001f;

struct IdleSource : gr::Block<IdleSource> {
    gr::PortOut<float> out;

    GR_MAKE_REFLECTABLE(IdleSource, out);

    gr::work::Status processBulk(gr::OutputSpanLike auto& outSpan) {
        outSpan.publish(0UZ);
        return gr::work::Status::INSUFFICIENT_INPUT_ITEMS;
    }
};

struct CountedSource : gr::Block<CountedSource> {
    gr::PortOut<float> out;

    GR_MAKE_REFLECTABLE(CountedSource, out);

    std::size_t _nEmitted = 0UZ;

    gr::work::Status processBulk(gr::OutputSpanLike auto& outSpan) {
        if (_nEmitted >= kLoadedSamples) {
            outSpan.publish(0UZ);
            return gr::work::Status::DONE;
        }
        const std::size_t n = std::min(outSpan.size(), kLoadedSamples - _nEmitted);
        for (std::size_t i = 0UZ; i < n; ++i) {
            outSpan[i] = 1.0f;
        }
        _nEmitted += n;
        outSpan.publish(n);
        return gr::work::Status::OK;
    }
};

struct Gain : gr::Block<Gain> {
    gr::PortIn<float>  in;
    gr::PortOut<float> out;

    GR_MAKE_REFLECTABLE(Gain, in, out);

    [[nodiscard]] constexpr float processOne(float value) const noexcept { return value * kGain; }
};

// publishes one chunk at the first call after kWakePause has passed since start(), then ends the stream
struct PolledSource : gr::Block<PolledSource> {
    gr::PortOut<float> out;

    GR_MAKE_REFLECTABLE(PolledSource, out);

    Clock::time_point _due{};
    Clock::time_point _publishedAt{};
    bool              _published = false;

    void start() { _due = Clock::now() + kWakePause; }

    gr::work::Status processBulk(gr::OutputSpanLike auto& outSpan) {
        if (_published) {
            outSpan.publish(0UZ);
            return gr::work::Status::DONE;
        }
        if (Clock::now() < _due || outSpan.size() < kWakeChunk) {
            outSpan.publish(0UZ);
            return gr::work::Status::INSUFFICIENT_INPUT_ITEMS;
        }
        for (std::size_t i = 0UZ; i < kWakeChunk; ++i) {
            outSpan[i] = 1.0f;
        }
        _publishedAt = Clock::now();
        _published   = true;
        outSpan.publish(kWakeChunk);
        return gr::work::Status::OK;
    }
};

// waits inside work() until kWakePause has passed since start() and, when `_awaitParkedSink` is set, until a worker
// waits on the progress sequence. Then it publishes one chunk and ends the stream.
struct HeldSource : gr::Block<HeldSource> {
    gr::PortOut<float> out;

    GR_MAKE_REFLECTABLE(HeldSource, out);

    bool              _awaitParkedSink = false;
    bool              _sinkParked      = false; // a worker waited on the progress sequence at the publication
    Clock::time_point _due{};
    Clock::time_point _publishedAt{};
    bool              _published = false;

    void start() { _due = Clock::now() + kWakePause; }

    gr::work::Status processBulk(gr::OutputSpanLike auto& outSpan) {
        if (_published) {
            outSpan.publish(0UZ);
            return gr::work::Status::DONE;
        }
        if (outSpan.size() < kWakeChunk) {
            outSpan.publish(0UZ);
            return gr::work::Status::INSUFFICIENT_OUTPUT_ITEMS;
        }
        std::this_thread::sleep_until(_due);
        if (_awaitParkedSink) {
            const Clock::time_point deadline = Clock::now() + kWakePause;
            while (this->progress->nTimedWaiters() == 0U && Clock::now() < deadline) {
                std::this_thread::sleep_for(std::chrono::microseconds(50));
            }
        }
        _sinkParked = this->progress->nTimedWaiters() > 0U;
        for (std::size_t i = 0UZ; i < kWakeChunk; ++i) {
            outSpan[i] = 1.0f;
        }
        _publishedAt = Clock::now();
        _published   = true;
        outSpan.publish(kWakeChunk);
        return gr::work::Status::OK;
    }
};

struct Sink : gr::Block<Sink> {
    gr::PortIn<float> in;

    GR_MAKE_REFLECTABLE(Sink, in);

    float             _acc       = 0.0f;
    std::size_t       _nReceived = 0UZ;
    Clock::time_point _firstAt{};

    gr::work::Status processBulk(gr::InputSpanLike auto& inSpan) {
        const std::size_t n = inSpan.size();
        if (_nReceived == 0UZ && n > 0UZ) {
            _firstAt = Clock::now();
        }
        for (std::size_t i = 0UZ; i < n; ++i) {
            _acc += inSpan[i];
        }
        _nReceived += n;
        std::ignore = inSpan.consume(n);
        return gr::work::Status::OK;
    }
};

[[nodiscard]] std::vector<bool> cpuMask(std::span<const std::size_t> cpus) {
    std::vector<bool> mask(std::max<std::size_t>(std::thread::hardware_concurrency(), std::ranges::max(cpus) + 1UZ), false);
    for (const std::size_t cpu : cpus) {
        mask[cpu] = true;
    }
    return mask;
}

[[nodiscard]] std::span<const std::size_t> cpusFor(ExecutionPolicy policy) { return gr::scheduler::isMultiThreaded(policy) ? std::span<const std::size_t>(kMultiCpus) : std::span<const std::size_t>(kSingleCpu); }

// pins the calling thread and reports whether the kernel accepted the CPU set
bool pinCallingThread(ExecutionPolicy policy) {
    try {
        gr::thread_pool::thread::setThreadAffinity(cpuMask(cpusFor(policy)));
        return true;
    } catch (const std::system_error&) {
        return false;
    }
}

void registerPool() {
    const auto nThreads = static_cast<std::uint32_t>(kMultiCpus.size());
    auto       pool     = std::make_shared<gr::thread_pool::ThreadPoolWrapper>(std::make_unique<gr::thread_pool::BasicThreadPool>(kPoolName, gr::thread_pool::TaskType::CPU_BOUND, nThreads, nThreads), "CPU");
    gr::thread_pool::Manager::instance().replacePool(std::string(kPoolName), pool);
}

[[nodiscard]] std::chrono::duration<double> processCpuTime() {
    rusage usage{};
    getrusage(RUSAGE_SELF, &usage);
    const auto seconds = [](const timeval& t) { return static_cast<double>(t.tv_sec) + 1e-6 * static_cast<double>(t.tv_usec); };
    return std::chrono::duration<double>(seconds(usage.ru_utime) + seconds(usage.ru_stime));
}

// runs runAndWait() on a thread pinned for the policy; `whileRunning` runs on the caller's thread meanwhile
template<ExecutionPolicy policy, typename TWhileRunning>
bool runPinned(gr::scheduler::Simple<policy>& scheduler, gr::Graph&& flow, TWhileRunning whileRunning) {
    if (!scheduler.exchange(std::move(flow)).has_value()) {
        std::println(stderr, "the scheduler refused the graph");
        return false;
    }
    std::atomic<bool> pinned{false};
    std::thread       runner([&scheduler, &pinned] {
        pinned.store(pinCallingThread(policy));
        std::ignore = scheduler.runAndWait();
    });
    whileRunning();
    runner.join();
    return pinned.load();
}

template<ExecutionPolicy policy>
[[nodiscard]] double idleCores(const gr::property_map& settings, bool& pinned) {
    gr::Graph flow;
    auto&     source = flow.emplaceBlock<IdleSource>();
    auto&     sink   = flow.emplaceBlock<Sink>();
    std::ignore      = flow.connect<"out", "in">(source, sink);

    gr::scheduler::Simple<policy> scheduler(settings);
    std::chrono::duration<double> cpu{};
    std::chrono::duration<double> wall{};
    pinned = runPinned(scheduler, std::move(flow), [&scheduler, &cpu, &wall] {
        while (scheduler.state() != gr::lifecycle::State::RUNNING) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        std::this_thread::sleep_for(kIdleSettle);
        const auto cpuBefore  = processCpuTime();
        const auto wallBefore = Clock::now();
        std::this_thread::sleep_for(kIdleWindow);
        cpu  = processCpuTime() - cpuBefore;
        wall = Clock::now() - wallBefore;
        scheduler.requestStop();
    });
    return cpu / wall;
}

template<ExecutionPolicy policy>
[[nodiscard]] std::size_t runChain(const gr::property_map& settings, std::size_t nChain, bool& pinned) {
    gr::Graph          flow;
    auto&              source = flow.emplaceBlock<CountedSource>();
    auto&              sink   = flow.emplaceBlock<Sink>();
    std::vector<Gain*> chain;
    for (std::size_t i = 0UZ; i < nChain; ++i) {
        chain.push_back(&flow.emplaceBlock<Gain>());
    }
    std::ignore = flow.connect<"out", "in">(source, *chain.front());
    for (std::size_t i = 1UZ; i < nChain; ++i) {
        std::ignore = flow.connect<"out", "in">(*chain[i - 1UZ], *chain[i]);
    }
    std::ignore = flow.connect<"out", "in">(*chain.back(), sink);

    gr::scheduler::Simple<policy> scheduler(settings);
    pinned = runPinned(scheduler, std::move(flow), [] {});
    return sink._nReceived;
}

// the no-scheduler arm of the idle rows: the same window with no graph running
[[nodiscard]] double floorCores() {
    std::this_thread::sleep_for(kIdleSettle);
    const auto cpuBefore  = processCpuTime();
    const auto wallBefore = Clock::now();
    std::this_thread::sleep_for(kIdleWindow);
    const std::chrono::duration<double> cpu  = processCpuTime() - cpuBefore;
    const std::chrono::duration<double> wall = Clock::now() - wallBefore;
    return cpu / wall;
}

// the time from the publication to the sink's receipt, with the sink's worker idle and, where the policy's pool workers
// park, parked; `sinkParked` reports whether a worker waited on the progress sequence at the publication
template<ExecutionPolicy policy>
[[nodiscard]] double wakeLatencyUs(const gr::property_map& settings, bool& pinned, bool& sinkParked) {
    gr::Graph flow;
    auto&     source        = flow.emplaceBlock<HeldSource>();
    auto&     sink          = flow.emplaceBlock<Sink>();
    source._awaitParkedSink = gr::scheduler::isMultiThreaded(policy) && gr::scheduler::Simple<policy>::parksIdleWorkers();
    std::ignore             = flow.connect<"out", "in">(source, sink);

    gr::scheduler::Simple<policy> scheduler(settings);
    pinned = runPinned(scheduler, std::move(flow), [] {});
    if (sink._nReceived != kWakeChunk) {
        std::println(stderr, "the sink received {} of {} samples", sink._nReceived, kWakeChunk);
    }
    sinkParked = source._sinkParked;
    return std::chrono::duration<double, std::micro>(sink._firstAt - source._publishedAt).count();
}

// the time from the due time of a source that polls the clock in work() to its publication
template<ExecutionPolicy policy>
[[nodiscard]] double pollDelayUs(const gr::property_map& settings, bool& pinned) {
    gr::Graph flow;
    auto&     source = flow.emplaceBlock<PolledSource>();
    auto&     sink   = flow.emplaceBlock<Sink>();
    std::ignore      = flow.connect<"out", "in">(source, sink);

    gr::scheduler::Simple<policy> scheduler(settings);
    pinned = runPinned(scheduler, std::move(flow), [] {});
    if (sink._nReceived != kWakeChunk) {
        std::println(stderr, "the sink received {} of {} samples", sink._nReceived, kWakeChunk);
    }
    return std::chrono::duration<double, std::micro>(source._publishedAt - source._due).count();
}

// the time from a settings request that the caller's thread sends into msgIn to the reply on msgOut, with the run idle
// and, where the policy parks idle workers, every worker parked; `allParked` reports whether they were
template<ExecutionPolicy policy>
[[nodiscard]] double messageReplyUs(const gr::property_map& settings, bool& pinned, bool& allParked) {
    gr::Graph flow;
    auto&     source = flow.emplaceBlock<IdleSource>();
    auto&     sink   = flow.emplaceBlock<Sink>();
    std::ignore      = flow.connect<"out", "in">(source, sink);

    gr::scheduler::Simple<policy> scheduler(settings);
    gr::MsgPortOut                toScheduler;
    gr::MsgPortIn                 fromScheduler;
    std::ignore = toScheduler.connect(scheduler.msgIn);
    std::ignore = scheduler.msgOut.connect(fromScheduler);

    // consumes every message waiting on the reply port and reports whether one answers `requestId`
    auto takeReply = [&fromScheduler](std::string_view requestId) {
        auto       messages = fromScheduler.streamReader().get();
        const bool found    = std::ranges::any_of(messages, [requestId](const gr::Message& message) { return message.clientRequestID == requestId; });
        std::ignore         = messages.consume(messages.size());
        return found;
    };

    double us = 0.0;
    pinned    = runPinned(scheduler, std::move(flow), [&] {
        while (scheduler.state() != gr::lifecycle::State::RUNNING) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        std::this_thread::sleep_for(kWakePause);
        if constexpr (gr::scheduler::Simple<policy>::parksIdleWorkers()) {
            const std::size_t       nWorkers = gr::scheduler::isMultiThreaded(policy) ? scheduler.jobs()->size() : 1UZ;
            const Clock::time_point deadline = Clock::now() + kWakePause;
            while (scheduler.graph().progress().nTimedWaiters() < nWorkers && Clock::now() < deadline) {
                std::this_thread::sleep_for(std::chrono::microseconds(50));
            }
            allParked = scheduler.graph().progress().nTimedWaiters() == nWorkers;
        }
        std::ignore                    = takeReply("");
        const Clock::time_point sentAt = Clock::now();
        gr::sendMessage<gr::message::Command::Get>(toScheduler, scheduler.unique_name, gr::block::property::kSetting, gr::property_map{}, "bm-request");
        bool replied = false;
        while (!replied && Clock::now() - sentAt < std::chrono::seconds(1)) {
            replied = takeReply("bm-request");
            if (!replied) {
                std::this_thread::yield();
            }
        }
        us = replied ? std::chrono::duration<double, std::micro>(Clock::now() - sentAt).count() : 1e6;
        scheduler.requestStop();
    });
    return us;
}

struct Arm {
    std::string_view          name;
    ExecutionPolicy           policy;
    std::optional<gr::Size_t> poolParkCount; // the scheduler's default when absent
};

[[nodiscard]] gr::property_map settingsFor(const Arm& arm) {
    gr::property_map settings{{"poolName", std::string(kPoolName)}};
    if (arm.poolParkCount.has_value()) {
        settings.insert_or_assign("pool_park_count", *arm.poolParkCount);
    }
    return settings;
}

// calls measure.operator()<policy>() with the arm's policy as a template argument
template<typename TMeasure>
void forPolicy(ExecutionPolicy policy, TMeasure&& measure) {
    using enum ExecutionPolicy;
    switch (policy) {
    case singleThreaded: measure.template operator()<singleThreaded>(); break;
    case singleThreadedBlocking: measure.template operator()<singleThreadedBlocking>(); break;
    case multiThreaded: measure.template operator()<multiThreaded>(); break;
    case multiThreadedBlocking: measure.template operator()<multiThreadedBlocking>(); break;
    }
}

// adds a row of the lowest value, the mean and the highest to the benchmark table, and with `eachRound` the value of
// each round in the columns "r1", "r2" and on
void addRow(std::string_view name, const std::vector<double>& values, double scale, std::string_view unit, bool eachRound = false) {
    auto& row = ::benchmark::results::add_result(name);
    if (values.empty()) {
        return;
    }
    const auto [low, high] = std::ranges::minmax(values);
    double sum             = 0.0;
    for (const double value : values) {
        sum += value;
    }
    row.try_emplace("#N", std::uint64_t{values.size()}, "", 0);
    row.try_emplace("min", static_cast<long double>(low * scale), std::string(unit), 2);
    row.try_emplace("mean", static_cast<long double>(sum / static_cast<double>(values.size()) * scale), std::string(unit), 2);
    row.try_emplace("max", static_cast<long double>(high * scale), std::string(unit), 2);
    if (eachRound) {
        for (std::size_t round = 0UZ; round < values.size(); ++round) {
            row.try_emplace(std::format("r{}", round + 1UZ), static_cast<long double>(values[round] * scale), std::string(unit), 3);
        }
    }
}

} // namespace bm_idle

inline const boost::ut::suite<"scheduler idle cost"> _scheduler_idle = [] {
    using namespace boost::ut;
    using namespace bm_idle;
    using enum gr::scheduler::ExecutionPolicy;

    registerPool();

    static const std::array<Arm, 5UZ> arms{{
        {"singleThreaded", singleThreaded, std::nullopt},
        {"singleThreadedBlocking", singleThreadedBlocking, std::nullopt},
        {"multiThreaded", multiThreaded, std::nullopt},
        {"multiThreadedBlocking", multiThreadedBlocking, std::nullopt},
        {"multiThreadedBlocking/park0", multiThreadedBlocking, 0U},
    }};

    for (const std::size_t nChain : {4UZ, 16UZ}) {
        for (const Arm& arm : arms) {
            const gr::property_map settings = settingsFor(arm);
            forPolicy(arm.policy, [&]<ExecutionPolicy policy>() { // the discarded run
                bool pinned = true;
                std::ignore = runChain<policy>(settings, nChain, pinned);
            });
            const std::string name                                    = std::format("throughput N={:<2} {}", nChain, arm.name);
            ::benchmark::benchmark<kLoadedRuns>(name, kLoadedSamples) = [&arm, &settings, nChain] {
                forPolicy(arm.policy, [&]<ExecutionPolicy policy>() {
                    bool pinned = true;
                    expect(eq(runChain<policy>(settings, nChain, pinned), kLoadedSamples));
                    expect(pinned) << arm.name << "start the process on a CPU set that holds the pinned CPUs";
                });
            };
        }
        ::benchmark::results::add_separator();
    }

    "idle cost, wake latency, poll delay and message reply per execution policy"_test = [] {
        std::array<std::vector<double>, arms.size()> idle{};
        std::vector<double>                          floorRounds;
        std::array<std::vector<double>, arms.size()> latency{};
        std::array<std::vector<double>, arms.size()> delay{};
        std::array<std::vector<double>, arms.size()> reply{};
        bool                                         allPinned = true;
        bool                                         allParked = true;

        for (std::size_t round = 0UZ; round <= kWakeRounds; ++round) {
            for (std::size_t i = 0UZ; i < arms.size(); ++i) {
                const std::size_t index = (round + i) % arms.size();
                forPolicy(arms[index].policy, [&]<ExecutionPolicy policy>() {
                    bool         pinned     = true;
                    bool         sinkParked = true;
                    const double us         = wakeLatencyUs<policy>(settingsFor(arms[index]), pinned, sinkParked);
                    const double delayUs    = pollDelayUs<policy>(settingsFor(arms[index]), pinned);
                    bool         idleParked = true;
                    const double replyUs    = messageReplyUs<policy>(settingsFor(arms[index]), pinned, idleParked);
                    allPinned               = allPinned && pinned;
                    if (gr::scheduler::isMultiThreaded(policy) && gr::scheduler::Simple<policy>::parksIdleWorkers()) {
                        allParked = allParked && sinkParked;
                    }
                    allParked = allParked && idleParked;
                    if (round > 0UZ) {
                        latency[index].push_back(us);
                        delay[index].push_back(delayUs);
                        reply[index].push_back(replyUs);
                    }
                });
            }
        }
        for (std::size_t round = 0UZ; round <= kIdleRounds; ++round) {
            for (std::size_t i = 0UZ; i <= arms.size(); ++i) {
                const std::size_t index = (round + i) % (arms.size() + 1UZ);
                if (index == arms.size()) {
                    const double cores = floorCores();
                    if (round > 0UZ) {
                        floorRounds.push_back(cores);
                    }
                    continue;
                }
                forPolicy(arms[index].policy, [&]<ExecutionPolicy policy>() {
                    bool         pinned = true;
                    const double cores  = idleCores<policy>(settingsFor(arms[index]), pinned);
                    allPinned           = allPinned && pinned;
                    if (round > 0UZ) {
                        idle[index].push_back(cores);
                    }
                });
            }
        }
        expect(allPinned) << "start the process on a CPU set that holds the pinned CPUs";
        expect(allParked) << "a worker was not parked at a publication or a request under a parking policy";

        addRow("idle CPU (cores) no scheduler", floorRounds, 1.0, "", true);
        for (std::size_t index = 0UZ; index < arms.size(); ++index) {
            addRow(std::format("idle CPU (cores) {}", arms[index].name), idle[index], 1.0, "", true);
        }
        ::benchmark::results::add_separator();
        for (std::size_t index = 0UZ; index < arms.size(); ++index) {
            addRow(std::format("wake latency {}", arms[index].name), latency[index], 1e-6, "s");
        }
        ::benchmark::results::add_separator();
        for (std::size_t index = 0UZ; index < arms.size(); ++index) {
            addRow(std::format("poll delay {}", arms[index].name), delay[index], 1e-6, "s");
        }
        ::benchmark::results::add_separator();
        for (std::size_t index = 0UZ; index < arms.size(); ++index) {
            addRow(std::format("message reply {}", arms[index].name), reply[index], 1e-6, "s");
        }
    };
};

int main() { /* not needed by the UT framework */ }
