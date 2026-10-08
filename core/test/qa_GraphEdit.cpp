#include <boost/ut.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <expected>
#include <format>
#include <functional>
#include <limits>
#include <map>
#include <memory_resource>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <vector>

#include <unistd.h>

#include <gnuradio-4.0/BlockRegistry.hpp>
#include <gnuradio-4.0/CircularBuffer.hpp>
#include <gnuradio-4.0/ComputeDomain.hpp>
#include <gnuradio-4.0/Graph.hpp>
#include <gnuradio-4.0/Graph_yaml_importer.hpp>
#include <gnuradio-4.0/Message.hpp>
#include <gnuradio-4.0/Scheduler.hpp>

namespace qa_edit {

struct Tunable : gr::Block<Tunable> {
    gr::PortIn<float>  in;
    gr::PortOut<float> out;

    gr::Annotated<float, "gain"> gain = 1.0f;

    GR_MAKE_REFLECTABLE(Tunable, in, out, gain);

    [[nodiscard]] constexpr float processOne(float value) const noexcept { return value * gain; }
};

struct Source : gr::Block<Source> {
    gr::PortOut<float> out;

    GR_MAKE_REFLECTABLE(Source, out);

    [[nodiscard]] constexpr float processOne() const noexcept { return 1.0f; }
};

struct CountingSource : gr::Block<CountingSource> {
    gr::PortOut<float> out;

    GR_MAKE_REFLECTABLE(CountingSource, out);

    static constexpr std::size_t kSamples = 4096UZ;

    std::size_t _nProduced = 0UZ;

    float processOne() {
        if (++_nProduced >= kSamples) {
            this->requestStop();
        }
        return 1.0f;
    }
};

// emits kSamples samples in every run and then reports DONE; each run ends by itself
struct FiniteSource : gr::Block<FiniteSource> {
    gr::PortOut<float> out;

    GR_MAKE_REFLECTABLE(FiniteSource, out);

    static constexpr std::size_t kSamples = 4096UZ;

    std::size_t _nProduced = 0UZ;

    void start() { _nProduced = 0UZ; }

    gr::work::Status processBulk(gr::OutputSpanLike auto& outSpan) {
        if (_nProduced >= kSamples) {
            outSpan.publish(0UZ);
            return gr::work::Status::DONE;
        }
        const std::size_t n = std::min(outSpan.size(), kSamples - _nProduced);
        std::fill_n(outSpan.begin(), n, 1.0f);
        outSpan.publish(n);
        _nProduced += n;
        return n == 0UZ ? gr::work::Status::INSUFFICIENT_OUTPUT_ITEMS : gr::work::Status::OK;
    }
};

struct Sink : gr::Block<Sink> {
    gr::PortIn<float> in;

    GR_MAKE_REFLECTABLE(Sink, in);

    std::size_t _nReceived = 0UZ;

    void processOne(float) { _nReceived++; }
};

// a stream block with one declared message output and one declared message input
struct EventBlock : gr::Block<EventBlock> {
    gr::PortIn<float>  in;
    gr::PortOut<float> out;
    gr::MsgPortIn      eventsIn;
    gr::MsgPortOut     eventsOut;

    GR_MAKE_REFLECTABLE(EventBlock, in, out, eventsIn, eventsOut);

    [[nodiscard]] constexpr float processOne(float value) const noexcept { return value; }
};

// one connected and one deliberately unconnected optional output
struct DualSource : gr::Block<DualSource> {
    gr::PortOut<float>               out;
    gr::PortOut<float, gr::Optional> monitor;

    GR_MAKE_REFLECTABLE(DualSource, out, monitor);

    gr::work::Status processBulk(gr::OutputSpanLike auto& outSpan, gr::OutputSpanLike auto& monitorSpan) {
        outSpan.publish(0UZ);
        monitorSpan.publish(0UZ);
        return gr::work::Status::DONE;
    }
};

// resolvable only through a test-local registry, never the global one, so a lookup that
// succeeds proves which loader served it
struct LoaderCanary : gr::Block<LoaderCanary> {
    gr::PortOut<float> out;

    GR_MAKE_REFLECTABLE(LoaderCanary, out);

    [[nodiscard]] constexpr float processOne() const noexcept { return 0.0f; }
};

using TestScheduler = gr::scheduler::Simple<gr::scheduler::ExecutionPolicy::multiThreaded>;

// ends its stream at its first call
struct EndingSource : gr::Block<EndingSource> {
    gr::PortOut<float> out;

    GR_MAKE_REFLECTABLE(EndingSource, out);

    gr::work::Status processBulk(gr::OutputSpanLike auto& outSpan) {
        outSpan.publish(0UZ);
        return gr::work::Status::DONE;
    }
};

constexpr std::string_view kLateFault = "the late source failed";

// reports kLateFault on its 50th call and ends its stream on its 300th
struct LateErrorSource : gr::Block<LateErrorSource> {
    gr::PortOut<float> out;

    GR_MAKE_REFLECTABLE(LateErrorSource, out);

    static constexpr std::size_t kCallsToError = 50UZ;
    static constexpr std::size_t kCallsToEnd   = 300UZ;

    std::size_t _nCalls = 0UZ;

    gr::work::Status processBulk(gr::OutputSpanLike auto& outSpan) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        if (++_nCalls == kCallsToError) {
            this->emitErrorMessage("processBulk", kLateFault);
        }
        outSpan.publish(0UZ);
        return _nCalls >= kCallsToEnd ? gr::work::Status::DONE : gr::work::Status::OK;
    }
};

inline std::atomic<std::size_t> gConstructions{0UZ};

// counts each instance it constructs in gConstructions. Its one output is optional and asynchronous
struct ConstructionCounter : gr::Block<ConstructionCounter> {
    gr::PortOut<float, gr::Async, gr::Optional> out;

    GR_MAKE_REFLECTABLE(ConstructionCounter, out);

    explicit ConstructionCounter(gr::property_map initParameters = {}) : gr::Block<ConstructionCounter>(std::move(initParameters)) { gConstructions.fetch_add(1UZ); }

    gr::work::Status processBulk(gr::OutputSpanLike auto& outSpan) {
        outSpan.publish(0UZ);
        return gr::work::Status::OK;
    }
};

inline std::atomic<bool>        gHoldRequested{false};
inline std::atomic<bool>        gHoldEntered{false};
inline std::atomic<bool>        gHoldEnded{false};
inline std::atomic<std::size_t> gConstructionsDuringHold{0UZ};

// Its first work() call after gHoldRequested is set reports gHoldEntered. The call watches gConstructions for 100 ms or
// until the first change, stores the change in gConstructionsDuringHold and reports gHoldEnded
struct HoldingProbe : gr::Block<HoldingProbe> {
    gr::PortOut<float, gr::Async, gr::Optional> out;

    GR_MAKE_REFLECTABLE(HoldingProbe, out);

    gr::work::Status processBulk(gr::OutputSpanLike auto& outSpan) {
        if (gHoldRequested.exchange(false)) {
            const std::size_t before = gConstructions.load();
            gHoldEntered.store(true);
            for (std::size_t i = 0UZ; i < 100UZ && gConstructions.load() == before; ++i) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            gConstructionsDuringHold.store(gConstructions.load() - before);
            gHoldEnded.store(true);
        }
        outSpan.publish(0UZ);
        return gr::work::Status::OK;
    }
};

inline std::atomic<std::size_t> gWorkingTickers{0UZ};

// counts itself once in gWorkingTickers at its first work() call. Its one output is optional and stays unconnected. The
// output is asynchronous. Each call reserves at most the free space in the port's buffer.
struct Ticker : gr::Block<Ticker> {
    gr::PortOut<float, gr::Async, gr::Optional> out;

    GR_MAKE_REFLECTABLE(Ticker, out);

    bool _counted = false;

    gr::work::Status processBulk(gr::OutputSpanLike auto& outSpan) {
        if (!_counted) {
            _counted = true;
            gWorkingTickers.fetch_add(1UZ);
        }
        outSpan.publish(0UZ);
        return gr::work::Status::OK;
    }
};

// reads the job lists and the count of workers that have not left
struct JobListProbe : TestScheduler {
    using TestScheduler::TestScheduler;

    [[nodiscard]] std::size_t nRunningJobs() const { return this->_nRunningJobs->value(); }

    [[nodiscard]] std::size_t nJobLists() {
        std::lock_guard lock(this->_executionOrderMutex);
        return this->_executionOrder->size();
    }

    [[nodiscard]] bool firstJobListHolds(std::string_view uniqueName) {
        std::lock_guard lock(this->_executionOrderMutex);
        return !this->_executionOrder->empty() && std::ranges::any_of(this->_executionOrder->front(), [uniqueName](const std::shared_ptr<gr::BlockModel>& block) { return block->uniqueName() == uniqueName; });
    }

    // the index of the job list that holds the named block
    [[nodiscard]] std::optional<std::size_t> jobListOf(std::string_view uniqueName) {
        std::lock_guard lock(this->_executionOrderMutex);
        for (std::size_t i = 0UZ; i < this->_executionOrder->size(); ++i) {
            if (std::ranges::any_of((*this->_executionOrder)[i], [uniqueName](const std::shared_ptr<gr::BlockModel>& block) { return block->uniqueName() == uniqueName; })) {
                return i;
            }
        }
        return std::nullopt;
    }
};

inline std::atomic<std::size_t> gCountedSamples{0UZ};

// a Tunable that counts the samples it passes in gCountedSamples
struct CountedTunable : gr::Block<CountedTunable> {
    gr::PortIn<float>  in;
    gr::PortOut<float> out;

    GR_MAKE_REFLECTABLE(CountedTunable, in, out);

    [[nodiscard]] float processOne(float value) noexcept {
        gCountedSamples.fetch_add(1UZ, std::memory_order_relaxed);
        return value;
    }
};

inline std::atomic<std::size_t> gHeldPublished{0UZ};
inline std::atomic<std::size_t> gHeldPassed{0UZ};
inline std::atomic<bool>        gSinkReleased{false};
inline std::atomic<std::size_t> gSinkReceived{0UZ};

// publishes kSamples samples, counted in gHeldPublished, and then nothing, without ending its stream
struct HeldSource : gr::Block<HeldSource> {
    gr::PortOut<float> out;

    GR_MAKE_REFLECTABLE(HeldSource, out);

    static constexpr std::size_t kSamples = 64UZ;

    gr::work::Status processBulk(gr::OutputSpanLike auto& outSpan) {
        const std::size_t nPublish = std::min(outSpan.size(), kSamples - gHeldPublished.load());
        for (std::size_t i = 0UZ; i < nPublish; ++i) {
            outSpan[i] = 1.0f;
        }
        outSpan.publish(nPublish);
        gHeldPublished.fetch_add(nPublish);
        return gr::work::Status::OK;
    }
};

// passes its first kPassed samples, counted in gHeldPassed, and keeps every later one at its input
struct HoldingStage : gr::Block<HoldingStage> {
    gr::PortIn<float>  in;
    gr::PortOut<float> out;

    GR_MAKE_REFLECTABLE(HoldingStage, in, out);

    static constexpr std::size_t kPassed = 16UZ;

    gr::work::Status processBulk(gr::InputSpanLike auto& inSpan, gr::OutputSpanLike auto& outSpan) {
        const std::size_t nPass = std::min({inSpan.size(), outSpan.size(), kPassed - gHeldPassed.load()});
        for (std::size_t i = 0UZ; i < nPass; ++i) {
            outSpan[i] = inSpan[i];
        }
        std::ignore = inSpan.consume(nPass);
        outSpan.publish(nPass);
        gHeldPassed.fetch_add(nPass);
        return gr::work::Status::OK;
    }
};

// takes no sample until gSinkReleased is set, then counts every sample it takes in gSinkReceived
struct HeldSink : gr::Block<HeldSink> {
    gr::PortIn<float> in;

    GR_MAKE_REFLECTABLE(HeldSink, in);

    gr::work::Status processBulk(gr::InputSpanLike auto& inSpan) {
        const std::size_t nTake = gSinkReleased.load() ? inSpan.size() : 0UZ;
        gSinkReceived.fetch_add(nTake);
        std::ignore = inSpan.consume(nTake);
        return gr::work::Status::OK;
    }
};

inline std::atomic<std::size_t> gFreeRunningSamples{0UZ};
inline std::atomic<std::size_t> gSamplesDuringStop{0UZ};
inline std::atomic<bool>        gStopProbed{false};

// counts every sample it takes in gFreeRunningSamples
struct FreeSink : gr::Block<FreeSink> {
    gr::PortIn<float> in;

    GR_MAKE_REFLECTABLE(FreeSink, in);

    void processOne(float) { gFreeRunningSamples.fetch_add(1UZ, std::memory_order_relaxed); }
};

// its stop() hook watches gFreeRunningSamples for 100 ms or until the first change, and stores the change in
// gSamplesDuringStop
struct StopProbe : gr::Block<StopProbe> {
    gr::PortOut<float, gr::Async, gr::Optional> out;

    GR_MAKE_REFLECTABLE(StopProbe, out);

    gr::work::Status processBulk(gr::OutputSpanLike auto& outSpan) {
        outSpan.publish(0UZ);
        return gr::work::Status::OK;
    }

    void stop() {
        const std::size_t before = gFreeRunningSamples.load();
        for (std::size_t i = 0UZ; i < 100UZ && gFreeRunningSamples.load() == before; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        gSamplesDuringStop.store(gFreeRunningSamples.load() - before);
        gStopProbed.store(true);
    }
};

// reads the state of the scheduler under test. A case sets it while its scheduler lives
inline std::function<gr::lifecycle::State()> gSchedulerState;
inline std::atomic<bool>                     gRacingEntered{false};

// a Tunable whose settingsChanged() reports that it has begun in gRacingEntered and returns once gSchedulerState reads a
// stopping scheduler. The scheduler applies the settings while it builds the block as a replacement
struct RacingStage : gr::Block<RacingStage> {
    gr::PortIn<float>  in;
    gr::PortOut<float> out;

    gr::Annotated<float, "gain"> gain = 1.0f;

    GR_MAKE_REFLECTABLE(RacingStage, in, out, gain);

    [[nodiscard]] constexpr float processOne(float value) const noexcept { return value * gain; }

    void settingsChanged(const gr::property_map&, const gr::property_map&) {
        if (!gSchedulerState) {
            return;
        }
        gRacingEntered.store(true);
        for (std::size_t i = 0UZ; i < 3000UZ && !gr::lifecycle::isShuttingDown(gSchedulerState()); ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
};

void registerTestBlocks() {
    static const bool registered = [] {
        std::ignore = gr::globalBlockRegistry().insert<Tunable>();
        std::ignore = gr::globalBlockRegistry().insert<Source>();
        std::ignore = gr::globalBlockRegistry().insert<Sink>();
        std::ignore = gr::globalBlockRegistry().insert<Ticker>();
        std::ignore = gr::globalBlockRegistry().insert<ConstructionCounter>();
        std::ignore = gr::globalBlockRegistry().insert<CountedTunable>();
        std::ignore = gr::globalBlockRegistry().insert<RacingStage>();
        return true;
    }();
    std::ignore = registered;
}

// stdout redirected into an anonymous file while this lives, so a case can read what the graph reported
struct StdoutCapture {
    std::FILE* _file    = std::tmpfile();
    int        _savedFd = -1;

    StdoutCapture() {
        std::fflush(stdout);
        _savedFd = ::dup(STDOUT_FILENO);
        ::dup2(::fileno(_file), STDOUT_FILENO);
    }

    StdoutCapture(const StdoutCapture&)            = delete;
    StdoutCapture& operator=(const StdoutCapture&) = delete;

    ~StdoutCapture() {
        std::fflush(stdout);
        ::dup2(_savedFd, STDOUT_FILENO);
        ::close(_savedFd);
        std::fclose(_file);
    }

    // pread leaves the offset alone, which stdout shares through dup2
    [[nodiscard]] std::string text() const {
        std::fflush(stdout);
        std::string           captured;
        std::array<char, 512> chunk{};
        for (ssize_t nRead = ::pread(::fileno(_file), chunk.data(), chunk.size(), 0); nRead > 0; nRead = ::pread(::fileno(_file), chunk.data(), chunk.size(), static_cast<off_t>(captured.size()))) {
            captured.append(chunk.data(), static_cast<std::size_t>(nRead));
        }
        return captured;
    }
};

// formats a port as the edge formatter prints it; the '/' keeps one block name from matching inside a longer one
[[nodiscard]] std::string portOf(std::string_view blockName, std::string_view port) { return std::format("{}/{}", blockName, port); }

[[nodiscard]] bool awaitReply(gr::MsgPortIn& port, std::string_view endpoint) {
    for (std::size_t i = 0UZ; i < 3000UZ; ++i) {
        auto messages = port.streamReader().get();
        for (const gr::Message& message : messages) {
            if (message.endpoint == endpoint) {
                return true;
            }
        }
        std::ignore = messages.consume(messages.size());
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return false;
}

[[nodiscard]] std::optional<gr::property_map> awaitReplyData(gr::MsgPortIn& port, std::string_view endpoint) {
    for (std::size_t i = 0UZ; i < 3000UZ; ++i) {
        auto messages = port.streamReader().get();
        for (const gr::Message& message : messages) {
            if (message.endpoint == endpoint && message.data.has_value()) {
                return message.data.value();
            }
        }
        std::ignore = messages.consume(messages.size());
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return std::nullopt;
}

[[nodiscard]] std::string awaitError(gr::MsgPortIn& port, std::string_view endpoint) {
    for (std::size_t i = 0UZ; i < 3000UZ; ++i) {
        auto messages = port.streamReader().get();
        for (const gr::Message& message : messages) {
            if (message.endpoint == endpoint && !message.data.has_value()) {
                return message.data.error().message;
            }
        }
        std::ignore = messages.consume(messages.size());
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return {};
}

// consumes every message it reads, so a later call sees only a later reply
[[nodiscard]] bool takeReply(gr::MsgPortIn& port, std::string_view endpoint) {
    for (std::size_t i = 0UZ; i < 3000UZ; ++i) {
        auto       messages = port.streamReader().get();
        const bool found    = std::ranges::any_of(messages, [endpoint](const gr::Message& message) { return message.endpoint == endpoint; });
        std::ignore         = messages.consume(messages.size());
        if (found) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return false;
}

template<typename TPredicate>
[[nodiscard]] bool awaitCondition(TPredicate satisfied) {
    for (std::size_t i = 0UZ; i < 3000UZ; ++i) {
        if (satisfied()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return satisfied();
}

// publishes nSamples samples of value 1 on an output port
void publishOnes(gr::PortOut<float>& port, std::size_t nSamples) {
    auto span = port.streamWriter().reserve(nSamples);
    std::ranges::fill(span, 1.0f);
    span.publish(nSamples);
}

// takes a block from the state its graph left it in to RUNNING. A case can then call its work()
void startForWork(gr::BlockModel& block) {
    if (block.state() == gr::lifecycle::State::IDLE) {
        std::ignore = block.changeStateTo(gr::lifecycle::State::INITIALISED);
    }
    std::ignore = block.changeStateTo(gr::lifecycle::State::RUNNING);
}

void sendMessage(gr::MsgPortOut& port, std::string_view endpoint, gr::property_map data) { gr::sendMessage<gr::message::Command::Set>(port, "", endpoint, std::move(data)); }

// the unique names of the graph's blocks in insertion order, comma separated
[[nodiscard]] std::string blockNames(const gr::Graph& graph) {
    std::string names;
    for (const std::shared_ptr<gr::BlockModel>& block : graph.blocks()) {
        names += std::format("{}{}", names.empty() ? "" : ", ", block->uniqueName());
    }
    return names;
}

// the message of the gr::exception that emplace throws, empty when it returns
[[nodiscard]] std::string refusal(auto emplace) {
    try {
        emplace();
    } catch (const gr::exception& e) {
        return e.message;
    }
    return {};
}

struct CountingResource : std::pmr::memory_resource {
    std::size_t nAllocations = 0UZ;

    void* do_allocate(std::size_t bytes, std::size_t alignment) override {
        ++nAllocations;
        return std::pmr::new_delete_resource()->allocate(bytes, alignment);
    }
    void do_deallocate(void* pointer, std::size_t bytes, std::size_t alignment) override { std::pmr::new_delete_resource()->deallocate(pointer, bytes, alignment); }
    bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override { return this == &other; }
};

// a compute-domain provider whose resource outlives every buffer bound to it
std::pmr::memory_resource* domainResource(const gr::ComputeDomain&, void*) {
    static CountingResource resource;
    return &resource;
}

// A block whose input no edge feeds never finishes. A run of such a graph is stopped once `done()` holds. `done()` is
// read only after the graph's progress sequence has moved, when the run has started its blocks. A block state left by
// an earlier run does not count. The wait gives up after kStallPolls polls without progress.
template<typename TScheduler, typename TDone>
[[nodiscard]] bool runUntil(TScheduler& scheduler, TDone done) {
    constexpr std::size_t kStallPolls = 5000UZ;

    const gr::Sequence& progress = scheduler.graph().progress();
    const std::size_t   before   = progress.value();
    std::thread         runner([&scheduler] { std::ignore = scheduler.runAndWait(); });
    std::size_t         seen     = before;
    std::size_t         nStalled = 0UZ;
    bool                reached  = false;
    while (!reached && nStalled < kStallPolls) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        const std::size_t current = progress.value();
        nStalled                  = current == seen ? nStalled + 1UZ : 0UZ;
        seen                      = current;
        reached                   = current != before && done();
    }
    std::ignore = scheduler.changeStateTo(gr::lifecycle::State::REQUESTED_STOP);
    runner.join();
    return reached;
}

// the unique names of a chain: a source feeding a Tunable stage feeding a sink, and a second sink without an edge
struct ChainNames {
    std::string source;
    std::string stage;
    std::string sink;
    std::string spareSink;
};

// gives the scheduler the chain and returns the names of its blocks
template<typename TScheduler>
[[nodiscard]] ChainNames exchangeChain(TScheduler& scheduler) {
    gr::Graph flow;
    auto&     source    = flow.emplaceBlock<Source>();
    auto&     stage     = flow.emplaceBlock<Tunable>();
    auto&     sink      = flow.emplaceBlock<Sink>();
    auto&     spareSink = flow.emplaceBlock<Sink>();
    boost::ut::expect(flow.connect<"out", "in">(source, stage).has_value());
    boost::ut::expect(flow.connect<"out", "in">(stage, sink).has_value());
    ChainNames names{std::string(source.unique_name), std::string(stage.unique_name), std::string(sink.unique_name), std::string(spareSink.unique_name)};
    boost::ut::expect(boost::ut::fatal(scheduler.exchange(std::move(flow)).has_value()));
    return names;
}

// the data of an EmplaceEdge request
[[nodiscard]] gr::property_map edgeRequest(std::string_view sourceBlock, std::string_view destinationBlock, gr::pmt::Value weight) {
    using namespace gr::serialization_fields;
    return {{std::pmr::string(EDGE_SOURCE_BLOCK), std::string(sourceBlock)}, {std::pmr::string(EDGE_SOURCE_PORT), std::string("out")}, //
        {std::pmr::string(EDGE_DESTINATION_BLOCK), std::string(destinationBlock)}, {std::pmr::string(EDGE_DESTINATION_PORT), std::string("in")}, {std::pmr::string(EDGE_MIN_BUFFER_SIZE), gr::undefined_Size}, {std::pmr::string(EDGE_WEIGHT), std::move(weight)}, {std::pmr::string(EDGE_NAME), std::string("requested")}};
}

// a request to the scheduler with the clientRequestID it carries, and whether the scheduler carries out the edit
struct EditRequest {
    std::string          id;
    std::string_view     endpoint;
    gr::property_map     data;
    bool                 succeeds = false;
    gr::message::Command cmd      = gr::message::Command::Set;
};

// sends every request to a scheduler that is not running and handles them on this thread. Each request must have one
// answer: Final, under replyEndpoint, with the request's clientRequestID, and with data when the edit succeeds or an
// error when the scheduler refuses it
template<typename TScheduler>
void expectRepliesUnder(TScheduler& scheduler, std::string_view replyEndpoint, std::span<const EditRequest> requests) {
    using namespace boost::ut;
    gr::MsgPortOut toScheduler;
    gr::MsgPortIn  fromScheduler;
    expect(fatal(toScheduler.connect(scheduler.msgIn).has_value()));
    expect(fatal(scheduler.msgOut.connect(fromScheduler).has_value()));

    for (const EditRequest& request : requests) {
        if (request.cmd == gr::message::Command::Get) {
            gr::sendMessage<gr::message::Command::Get>(toScheduler, scheduler.unique_name, request.endpoint, request.data, request.id);
        } else {
            gr::sendMessage<gr::message::Command::Set>(toScheduler, scheduler.unique_name, request.endpoint, request.data, request.id);
        }
    }
    scheduler.processScheduledMessages();

    auto replies = fromScheduler.streamReader().get();
    for (const EditRequest& request : requests) {
        auto answersRequest = [&request](const gr::Message& reply) { return reply.clientRequestID == request.id; };
        expect(eq(static_cast<std::size_t>(std::ranges::count_if(replies, answersRequest)), 1UZ)) << std::format("'{}': the number of replies with the request's id", request.id);
        const auto reply = std::ranges::find_if(replies, answersRequest);
        if (reply == replies.end()) {
            continue;
        }
        expect(reply->cmd == gr::message::Command::Final) << std::format("'{}': the reply is not Final", request.id);
        expect(eq(reply->endpoint, std::string(replyEndpoint))) << std::format("'{}': the reply's endpoint", request.id);
        expect(eq(reply->data.has_value(), request.succeeds)) << std::format("'{}': {}", request.id, reply->data.has_value() ? std::string("the refusal was answered with data") : reply->data.error().message);
    }
    std::ignore = replies.consume(replies.size());
}

// the reply that carries the clientRequestID, if one arrives. Consumes every message it reads
[[nodiscard]] std::optional<gr::Message> takeReplyTo(gr::MsgPortIn& port, std::string_view clientRequestID) {
    for (std::size_t i = 0UZ; i < 3000UZ; ++i) {
        auto                       messages = port.streamReader().get();
        std::optional<gr::Message> reply;
        if (const auto it = std::ranges::find_if(messages, [clientRequestID](const gr::Message& message) { return message.clientRequestID == clientRequestID; }); it != messages.end()) {
            reply = *it;
        }
        std::ignore = messages.consume(messages.size());
        if (reply.has_value()) {
            return reply;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return std::nullopt;
}

// the job list index and the unique name that a BlockReplaced reply names, each empty when the reply lacks it
struct ReplacementRecord {
    std::optional<std::size_t> jobList;
    std::string                uniqueName;
};

[[nodiscard]] ReplacementRecord replacementRecord(const gr::property_map& reply) {
    ReplacementRecord record;
    if (const auto it = reply.find("jobList"); it != reply.end()) {
        if (const auto* index = it->second.get_if<gr::Size_t>(); index != nullptr) {
            record.jobList = static_cast<std::size_t>(*index);
        }
    }
    if (const auto it = reply.find(std::pmr::string(gr::serialization_fields::BLOCK_UNIQUE_NAME)); it != reply.end()) {
        record.uniqueName = it->second.value_or(std::string());
    }
    return record;
}

// Runs HeldSource -> HoldingStage -> HeldSink until the source has published its samples and the stage has passed its
// share, which the sink holds. The case then replaces the stage with a Tunable and releases the sink. Every sample
// reaches the sink once: the replacement passes the samples queued at its input, and the sink reads those queued at
// its output
template<typename TScheduler>
void expectQueuedSamplesPassReplacement() {
    using namespace boost::ut;
    using enum gr::lifecycle::State;
    registerTestBlocks();
    gHeldPublished.store(0UZ);
    gHeldPassed.store(0UZ);
    gSinkReleased.store(false);
    gSinkReceived.store(0UZ);

    gr::Graph flow;
    auto&     source = flow.emplaceBlock<HeldSource>();
    auto&     stage  = flow.emplaceBlock<HoldingStage>();
    auto&     sink   = flow.emplaceBlock<HeldSink>();
    expect(flow.connect<"out", "in">(source, stage).has_value());
    expect(flow.connect<"out", "in">(stage, sink).has_value());
    const std::string stageName{stage.unique_name};

    TScheduler scheduler;
    expect(fatal(scheduler.exchange(std::move(flow)).has_value()));
    gr::MsgPortOut toScheduler;
    gr::MsgPortIn  fromScheduler;
    expect(fatal(toScheduler.connect(scheduler.msgIn).has_value()));
    expect(fatal(scheduler.msgOut.connect(fromScheduler).has_value()));

    std::thread runner([&scheduler] { std::ignore = scheduler.runAndWait(); }); // a single-threaded run executes on the thread that starts it
    expect(awaitCondition([] { return gHeldPublished.load() == HeldSource::kSamples && gHeldPassed.load() == HoldingStage::kPassed; })) << "the stage did not hold samples at both its ports";

    sendMessage(toScheduler, gr::scheduler::property::kReplaceBlock, {{"uniqueName", stageName}, {"type", gr::meta::type_name<Tunable>()}});
    expect(awaitReplyData(fromScheduler, gr::scheduler::property::kBlockReplaced).has_value()) << "the replacement was refused";
    gSinkReleased.store(true);
    expect(awaitCondition([] { return gSinkReceived.load() == HeldSource::kSamples; })) << std::format("the sink received {} of {} samples", gSinkReceived.load(), HeldSource::kSamples);

    expect(scheduler.changeStateTo(REQUESTED_STOP).has_value());
    runner.join();
    expect(eq(gSinkReceived.load(), HeldSource::kSamples)) << "the sink received a sample twice";
}

} // namespace qa_edit

const boost::ut::suite<"graph editing"> graphEditTests = [] {
    using namespace boost::ut;
    using enum gr::lifecycle::State;

#ifndef GR_TEST_WITHOUT_BLOCK_REGISTRY // emplacement by name resolves the type through the registry
    "a block emplaced from yaml applies its serialized settings"_test = [] {
        qa_edit::registerTestBlocks();

        qa_edit::TestScheduler scheduler;
        {
            gr::Graph flow;
            auto&     source = flow.emplaceBlock<qa_edit::Source>();
            auto&     sink   = flow.emplaceBlock<qa_edit::Sink>();
            expect(flow.connect<"out", "in">(source, sink).has_value());
            expect(scheduler.exchange(std::move(flow)).has_value());
        }

        gr::MsgPortOut toScheduler;
        gr::MsgPortIn  fromScheduler;
        expect(toScheduler.connect(scheduler.msgIn).has_value());
        expect(scheduler.msgOut.connect(fromScheduler).has_value());

        const std::string blockYaml = std::format("id: {}\nparameters:\n  gain: !!float32 4.5\n", gr::meta::type_name<qa_edit::Tunable>());
        qa_edit::sendMessage(toScheduler, gr::scheduler::property::kEmplaceBlock, {{"yaml", blockYaml}});

        expect(scheduler.changeStateTo(INITIALISED).has_value());
        expect(scheduler.changeStateTo(RUNNING).has_value());
        expect(qa_edit::awaitReply(fromScheduler, gr::scheduler::property::kBlockEmplaced)) << "the block was never emplaced";

        const auto* emplaced = [&]() -> const gr::BlockModel* {
            for (const auto& block : scheduler.graph().blocks()) {
                if (block->typeName().find("Tunable") != std::string_view::npos) {
                    return block.get();
                }
            }
            return nullptr;
        }();
        expect(fatal(emplaced != nullptr)) << "the emplaced block is not in the graph";

        const auto gain = emplaced->settings().get("gain");
        expect(fatal(gain.has_value())) << "the emplaced block reports no gain setting";
        expect(eq(gain->value_or(0.0f), 4.5f)) << "the emplaced block kept its constructor default instead of the serialized value";

        expect(scheduler.changeStateTo(REQUESTED_STOP).has_value());
    };

    "a subgraph emplaced by name inherits its parent's plugin loader"_test = [] {
        gr::BlockRegistry     localRegistry;
        gr::SchedulerRegistry localSchedulers;
        std::ignore = localRegistry.insert<qa_edit::LoaderCanary>();
        gr::PluginLoader localLoader(localRegistry, localSchedulers, {});

        const std::string canaryType{gr::meta::type_name<qa_edit::LoaderCanary>()};
        expect(gr::globalPluginLoader().instantiate(canaryType) == nullptr) << "the canary resolves globally, so this test cannot discriminate the loaders";

        gr::Graph                              flow(localLoader);
        const std::shared_ptr<gr::BlockModel>& wrapped = flow.emplaceBlock("gr::Graph", {{"name", std::string("inner")}});
        expect(fatal(wrapped != nullptr));
        expect(eq(std::string{wrapped->name()}, std::string{"inner"})) << "the emplaced subgraph dropped its settings";

        gr::Graph* inner = wrapped->graph();
        expect(fatal(inner != nullptr));
        expect(inner->_pluginLoader == &localLoader) << "the nested graph bound a loader other than its parent's";
        expect(nothrow([&] { std::ignore = inner->emplaceBlock(canaryType, {}); })) << "a type the parent's loader resolves must resolve inside the subgraph";

        auto& toReplace = flow.emplaceBlock<qa_edit::Source>();
        expect(nothrow([&] { std::ignore = flow.replaceBlock(toReplace.unique_name, canaryType, {}); })) << "replaceBlock must consult the graph's own loader";
    };

    // A pool of two threads gives the run two job lists. The probe holds one work() call in the first list, and the
    // worker of the second list handles the request meanwhile
    "a subgraph emplaced from yaml in a running graph loads while no block of the run is inside work()"_test = [] {
        constexpr std::string_view kPoolName = "qa_edit_two_threads";
        gr::thread_pool::Manager::instance().replacePool(std::string(kPoolName), std::make_shared<gr::thread_pool::ThreadPoolWrapper>(std::make_unique<gr::thread_pool::BasicThreadPool>(kPoolName, gr::thread_pool::TaskType::CPU_BOUND, 2U, 2U), "CPU"));
        qa_edit::registerTestBlocks();
        qa_edit::gHoldRequested.store(false);
        qa_edit::gHoldEntered.store(false);
        qa_edit::gHoldEnded.store(false);
        qa_edit::gConstructionsDuringHold.store(0UZ);

        gr::Graph flow;
        auto&     probe  = flow.emplaceBlock<qa_edit::HoldingProbe>();
        auto&     source = flow.emplaceBlock<qa_edit::Source>();
        auto&     sink   = flow.emplaceBlock<qa_edit::Sink>();
        expect(flow.connect<"out", "in">(source, sink).has_value());
        const std::string probeName{probe.unique_name};

        qa_edit::JobListProbe scheduler({{"poolName", std::string(kPoolName)}});
        expect(fatal(scheduler.exchange(std::move(flow)).has_value()));
        gr::MsgPortOut toScheduler;
        gr::MsgPortIn  fromScheduler;
        expect(fatal(toScheduler.connect(scheduler.msgIn).has_value()));
        expect(fatal(scheduler.msgOut.connect(fromScheduler).has_value()));
        expect(scheduler.changeStateTo(INITIALISED).has_value());
        expect(scheduler.changeStateTo(RUNNING).has_value());
        expect(fatal(eq(scheduler.nJobLists(), 2UZ))) << "the graph was not split into two job lists";
        expect(fatal(scheduler.firstJobListHolds(probeName))) << "the probe is not in the first job list";

        qa_edit::gHoldRequested.store(true);
        expect(fatal(qa_edit::awaitCondition([] { return qa_edit::gHoldEntered.load(); }))) << "the probe never held a work() call";
        const std::string subgraphYaml = std::format("id: SUBGRAPH\nparameters:\n  name: group\ngraph:\n  blocks:\n    - id: {}\n      parameters:\n        name: inner\n", gr::meta::type_name<qa_edit::ConstructionCounter>());
        qa_edit::sendMessage(toScheduler, gr::scheduler::property::kEmplaceBlock, {{"yaml", subgraphYaml}});
        expect(qa_edit::awaitReplyData(fromScheduler, gr::scheduler::property::kBlockEmplaced).has_value()) << "the subgraph was refused";
        expect(fatal(qa_edit::awaitCondition([] { return qa_edit::gHoldEnded.load(); }))) << "the probe's held call never returned";
        expect(eq(qa_edit::gConstructionsDuringHold.load(), 0UZ)) << "the subgraph loaded while the probe was inside work()";

        expect(scheduler.changeStateTo(REQUESTED_STOP).has_value());
        expect(qa_edit::awaitCondition([&scheduler] { return scheduler.state() == STOPPED; })) << "the scheduler did not stop";
    };

    "a block emplaced by name with a setting it does not declare stays out of the graph"_test = [] {
        qa_edit::registerTestBlocks();
        const std::string tunableType{gr::meta::type_name<qa_edit::Tunable>()};

        gr::Graph flow;
        std::ignore                     = flow.emplaceBlock<qa_edit::Source>();
        const std::size_t nBlocksBefore = flow.blocks().size();
        const std::string namesBefore   = qa_edit::blockNames(flow);

        const std::string refused = qa_edit::refusal([&] { std::ignore = flow.emplaceBlock(tunableType, {{"gian", 2.0f}}); });
        expect(refused.starts_with("settings could not be applied") && refused.contains("gian")) << "the refusal must reach the caller as thrown, got: " << refused;
        expect(eq(flow.blocks().size(), nBlocksBefore)) << "the block stayed in the graph after throwing: " << refused;
        expect(eq(qa_edit::blockNames(flow), namesBefore)) << "the graph holds other blocks after throwing: " << refused;

        const std::shared_ptr<gr::BlockModel>& accepted = flow.emplaceBlock(tunableType, {{"gain", 2.0f}});
        expect(eq(flow.blocks().size(), nBlocksBefore + 1UZ)) << "a block with settings it declares must join the graph";
        const std::optional<gr::pmt::Value> gain = accepted->settings().get("gain");
        expect(fatal(gain.has_value())) << "the accepted block reports no gain setting";
        expect(eq(gain->value_or(0.0f), 2.0f)) << "the accepted block must apply its settings";
    };

    "a block emplaced by name with a value its setting refuses stays out of the graph"_test = [] {
        qa_edit::registerTestBlocks();
        const std::string tunableType{gr::meta::type_name<qa_edit::Tunable>()};

        gr::Graph flow;
        std::ignore                     = flow.emplaceBlock<qa_edit::Source>();
        const std::size_t nBlocksBefore = flow.blocks().size();
        const std::string namesBefore   = qa_edit::blockNames(flow);

        const std::string refused = qa_edit::refusal([&] { std::ignore = flow.emplaceBlock(tunableType, {{"gain", std::string("loud")}}); });
        expect(refused.contains("'gain'")) << "the refusal must reach the caller as thrown, got: " << refused;
        expect(eq(flow.blocks().size(), nBlocksBefore)) << "the block stayed in the graph after throwing: " << refused;
        expect(eq(qa_edit::blockNames(flow), namesBefore)) << "the graph holds other blocks after throwing: " << refused;
    };

    "a replacement takes over the samples queued at the ports of the block it replaces"_test = [] {
        constexpr std::size_t kQueuedAtInput  = 5UZ;
        constexpr std::size_t kQueuedAtOutput = 3UZ;
        qa_edit::registerTestBlocks();

        gr::Graph flow;
        auto&     source = flow.emplaceBlock<qa_edit::Source>();
        auto&     stage  = flow.emplaceBlock<qa_edit::Tunable>();
        auto&     sink   = flow.emplaceBlock<qa_edit::Sink>();
        expect(flow.connect<"out", "in">(source, stage).has_value());
        expect(flow.connect<"out", "in">(stage, sink).has_value());
        expect(fatal(flow.connectPendingEdges())) << "the graph did not connect";
        qa_edit::publishOnes(source.out, kQueuedAtInput);
        qa_edit::publishOnes(stage.out, kQueuedAtOutput);
        const std::shared_ptr<gr::BlockModel> sinkModel = flow.blocks()[2UZ];

        auto [oldBlock, newBlock] = flow.replaceBlock(stage.unique_name, gr::meta::type_name<qa_edit::Tunable>(), {{"gain", 2.0f}});
        expect(fatal(newBlock != nullptr));
        expect(flow.blocks()[1UZ] == newBlock) << "the replacement does not hold the place of the replaced block";
        expect(!stage.in.isConnected()) << "the replaced block still reads the source's ring";
        expect(!stage.out.isConnected()) << "the sink still reads the replaced block's ring";
        for (const gr::Edge& edge : flow.edges()) {
            expect(edge.sourceBlock() != oldBlock && edge.destinationBlock() != oldBlock) << std::format("{}", edge) << ": the edge names the replaced block";
            expect(edge._sourcePort == edge.sourceBlock()->dynamicOutputPort("out").value()) << std::format("{}", edge) << ": the edge names another source port";
            expect(edge._destinationPort == edge.destinationBlock()->dynamicInputPort("in").value()) << std::format("{}", edge) << ": the edge names another destination port";
        }

        oldBlock.reset();
        qa_edit::startForWork(*newBlock);
        qa_edit::startForWork(*sinkModel);
        std::ignore = newBlock->work(std::numeric_limits<std::size_t>::max());
        std::ignore = sinkModel->work(std::numeric_limits<std::size_t>::max());
        expect(eq(sink._nReceived, kQueuedAtInput + kQueuedAtOutput)) << "the sink did not receive the samples queued at the replaced block's ports";
    };

    "a port takes the rings of a port that has not built its own and of one that has"_test = [] {
        constexpr std::size_t kQueued = 5UZ;

        gr::PortOut<float> source;
        gr::PortIn<float>  queued;
        gr::PortIn<float>  unbuilt;
        gr::PortIn<float>  built;
        gr::DynamicPort    sourcePort(source, gr::DynamicPort::non_owned_reference_tag{});
        gr::DynamicPort    queuedPort(queued, gr::DynamicPort::non_owned_reference_tag{});
        gr::DynamicPort    unbuiltPort(unbuilt, gr::DynamicPort::non_owned_reference_tag{});
        gr::DynamicPort    builtPort(built, gr::DynamicPort::non_owned_reference_tag{});
        expect(fatal(sourcePort.connect(queuedPort).has_value()));
        qa_edit::publishOnes(source, kQueued);
        expect(fatal(eq(built.streamReader().available(), 0UZ))); // the reader handed out builds the default rings

        const auto intoUnbuilt = queuedPort.checkHandlerExchange(unbuiltPort);
        expect(fatal(intoUnbuilt.has_value())) << "refused for a port without rings: " << (intoUnbuilt.has_value() ? std::string{} : intoUnbuilt.error().message);
        queuedPort.exchangeHandlers(unbuiltPort);
        expect(fatal(unbuilt.isConnected())) << "the port without rings did not take the connected ring";
        expect(eq(unbuilt.streamReader().available(), kQueued)) << "the queued samples did not move";
        expect(!queued.isConnected()) << "the port that gave up its rings still reads the source";
        expect(eq(source.nReaders(), 1UZ)) << "the exchange changed the source's readers";

        const auto intoBuilt = unbuiltPort.checkHandlerExchange(builtPort);
        expect(fatal(intoBuilt.has_value())) << "refused for a port with rings: " << (intoBuilt.has_value() ? std::string{} : intoBuilt.error().message);
        unbuiltPort.exchangeHandlers(builtPort);
        expect(fatal(built.isConnected())) << "the port with its own rings did not take the connected ring";
        expect(eq(built.streamReader().available(), kQueued)) << "the queued samples did not move";
        expect(eq(unbuilt.streamReader().available(), 0UZ)) << "the port that gave up the connected ring kept its samples";
        expect(eq(source.nReaders(), 1UZ)) << "the exchange changed the source's readers";
    };

    "a replacement that lacks a port of the replaced block is refused and the graph is unchanged"_test = [] {
        constexpr std::size_t kQueuedAtInput = 5UZ;
        qa_edit::registerTestBlocks();

        gr::Graph flow;
        auto&     source = flow.emplaceBlock<qa_edit::Source>();
        auto&     stage  = flow.emplaceBlock<qa_edit::Tunable>();
        auto&     sink   = flow.emplaceBlock<qa_edit::Sink>();
        expect(flow.connect<"out", "in">(source, stage).has_value());
        expect(flow.connect<"out", "in">(stage, sink).has_value());
        expect(fatal(flow.connectPendingEdges())) << "the graph did not connect";
        qa_edit::publishOnes(source.out, kQueuedAtInput);
        const std::string namesBefore = qa_edit::blockNames(flow);

        const std::string refused = qa_edit::refusal([&] { std::ignore = flow.replaceBlock(stage.unique_name, gr::meta::type_name<qa_edit::Source>(), {}); });
        expect(!refused.empty()) << "a replacement without an input port was accepted";
        expect(eq(qa_edit::blockNames(flow), namesBefore)) << "a refused replacement changed the blocks: " << refused;
        expect(eq(flow.edges()[0UZ].destinationBlock()->uniqueName(), std::string_view(stage.unique_name))) << "a refused replacement repointed an edge";
        expect(eq(stage.in.streamReader().available(), kQueuedAtInput)) << "a refused replacement moved the queued samples";
        expect(eq(sink.in.nWriters(), 1UZ)) << "a refused replacement moved the sink's ring";
    };
#endif

    "a typed emplacement with a setting the block does not declare leaves the graph as it was"_test = [] {
        gr::Graph flow;
        std::ignore                     = flow.emplaceBlock<qa_edit::Source>();
        const std::size_t nBlocksBefore = flow.blocks().size();
        const std::string namesBefore   = qa_edit::blockNames(flow);

        const std::string refused = qa_edit::refusal([&] { std::ignore = flow.emplaceBlock<qa_edit::Tunable>({{"gian", 2.0f}}); });
        expect(refused.starts_with("settings could not be applied") && refused.contains("gian")) << "the refusal must reach the caller as thrown, got: " << refused;
        expect(eq(flow.blocks().size(), nBlocksBefore)) << "the block stayed in the graph after throwing: " << refused;
        expect(eq(qa_edit::blockNames(flow), namesBefore)) << "the graph holds other blocks after throwing: " << refused;

        auto& accepted = flow.emplaceBlock<qa_edit::Tunable>({{"gain", 2.0f}});
        expect(eq(flow.blocks().size(), nBlocksBefore + 1UZ)) << "a block with settings it declares must join the graph";
        expect(eq(accepted.gain.value, 2.0f)) << "the accepted block must apply its settings";
        expect(accepted.state() == INITIALISED) << "the accepted block must be initialized";
    };

    "an exported output with interior consumers feeds both sides of the boundary"_test = [] {
        gr::Graph flow;
        auto      wrapper   = std::make_shared<gr::GraphWrapper<gr::Graph>>();
        auto&     inner     = *wrapper->graph();
        auto&     producer  = inner.emplaceBlock<qa_edit::CountingSource>();
        auto&     innerSink = inner.emplaceBlock<qa_edit::Sink>();
        expect(inner.connect<"out", "in">(producer, innerSink).has_value());

        const std::shared_ptr<gr::BlockModel>& subgraph = flow.addBlock(wrapper);
        expect(wrapper->exportPort(true, producer.unique_name, gr::PortDirection::OUTPUT, "out", "out").has_value());
        std::ignore = flow.emplaceBlock<qa_edit::Sink>();
        expect(flow.connect(subgraph, gr::PortDefinition{"out"}, flow.blocks()[1], gr::PortDefinition{"in"}).has_value());

        expect(flow.edges()[0].hasSameSourcePort(inner.edges()[0])) << "the exported alias and the interior edge reference the same port and must compare equal";

        // the wiring order a scheduler uses: the top-level graph's edges, then the subgraph's
        expect(flow.connectPendingEdges());
        expect(inner.connectPendingEdges());

        expect(eq(producer.out.nReaders(), 2UZ)) << "the boundary split the fan-out across two buffers, so one consumer starves";
    };

    "an unconnected optional output is sized with its connected sibling"_test = [] {
        gr::Graph flow;
        auto&     source = flow.emplaceBlock<qa_edit::DualSource>();
        auto&     sink   = flow.emplaceBlock<qa_edit::Sink>();
        expect(flow.connect<"out", "in">(source, sink).has_value());
        expect(flow.connectPendingEdges());
        expect(eq(source.monitor.bufferSize(), source.out.bufferSize())) << "a span request past the default capacity returns empty with nothing signaled";
    };

    "an edge naming no memory resource gets the buffer's own default"_test = [] {
        gr::Graph flow;
        auto&     source = flow.emplaceBlock<qa_edit::Source>();
        auto&     sink   = flow.emplaceBlock<qa_edit::Sink>();
        expect(flow.connect<"out", "in">(source, sink).has_value());
        expect(fatal(eq(flow.edges().size(), 1UZ)));
        expect(flow.connectPendingEdges());

        const gr::Edge& edge = flow.edges()[0];
        expect(edge._dataResource == nullptr) << "an edge nobody gave a resource must leave the choice to the buffer";
        expect(edge._tagResource == nullptr) << "an edge nobody gave a resource must leave the choice to the buffer";
        expect(!source.out.buffer().tagBuffer.isMmapAllocated()) << "a tag is not trivially copyable, so its ring stays on the heap";
        if constexpr (gr::has_posix_mmap_interface) {
            expect(source.out.buffer().streamBuffer.isMmapAllocated()) << "a trivially copyable sample on a platform with mmap must get the double-mapped ring";
        }
    };

    "an edge naming the default resource gets a heap buffer"_test = [] {
        gr::Graph flow;
        auto&     source = flow.emplaceBlock<qa_edit::Source>();
        auto&     sink   = flow.emplaceBlock<qa_edit::Sink>();
        expect(flow.connect<"out", "in">(source, sink, gr::EdgeParameters{.dataResource = std::pmr::get_default_resource(), .tagResource = std::pmr::get_default_resource()}).has_value());
        expect(flow.connectPendingEdges());

        expect(flow.edges()[0]._dataResource == std::pmr::get_default_resource()) << "a named resource must stay as named";
        expect(source.out.dataResource() == std::pmr::get_default_resource()) << "the port's buffer must come from the named resource";
        expect(!source.out.buffer().streamBuffer.isMmapAllocated()) << "a named heap resource must not be replaced by the double-mapped ring";
    };

#ifndef GR_TEST_WITHOUT_BLOCK_REGISTRY // a graph file resolves its block ids through the registry
    "an edge whose graph file names a memory resource keeps it"_test = [] {
        qa_edit::registerTestBlocks();
        gr::ComputeRegistry::instance().register_provider("qa-edit-graph-file", &qa_edit::domainResource);
        auto&             resource        = static_cast<qa_edit::CountingResource&>(*qa_edit::domainResource({}, nullptr));
        const std::size_t nAllocationsOld = resource.nAllocations;

        const std::string graphFile = std::format("blocks:\n"
                                                  "  - id: {}\n"
                                                  "    parameters:\n"
                                                  "      name: source\n"
                                                  "      compute_domain: \"gpu:qa-edit-graph-file\"\n"
                                                  "  - id: {}\n"
                                                  "    parameters:\n"
                                                  "      name: sink\n"
                                                  "connections:\n"
                                                  "  - [source, out, sink, in]\n",
            gr::meta::type_name<qa_edit::Source>(), gr::meta::type_name<qa_edit::Sink>());
        auto              flow      = gr::loadGrc(gr::globalPluginLoader(), graphFile);
        expect(fatal(eq(flow->edges().size(), 1UZ)));
        expect(flow->connectPendingEdges());

        const gr::Edge& edge = flow->edges()[0];
        expect(edge._dataResource == &resource) << "the resource the graph file's compute domain names must reach the edge";
        expect(edge._tagResource == &resource) << "the resource the graph file's compute domain names must reach the edge";
        auto& source = *static_cast<qa_edit::Source*>(edge._sourceBlock->raw());
        expect(source.out.dataResource() == &resource) << "the port's buffer must come from the named resource";
        expect(gt(resource.nAllocations, nAllocationsOld)) << "the named resource must have served the buffer";
    };
#endif

    "an edge naming only its tag resource keeps it while the compute domain fills the data resource"_test = [] {
        gr::ComputeRegistry::instance().register_provider("qa-edit-mixed", &qa_edit::domainResource);
        auto&                     domainPool = static_cast<qa_edit::CountingResource&>(*qa_edit::domainResource({}, nullptr));
        qa_edit::CountingResource tagPool;
        gr::Graph                 flow;
        auto&                     source = flow.emplaceBlock<qa_edit::Source>({{"compute_domain", std::string("gpu:qa-edit-mixed")}});
        auto&                     sink   = flow.emplaceBlock<qa_edit::Sink>();
        expect(flow.connect<"out", "in">(source, sink, gr::EdgeParameters{.tagResource = &tagPool}).has_value());
        expect(flow.connectPendingEdges());

        const gr::Edge& edge = flow.edges()[0];
        expect(edge._tagResource == &tagPool) << "a tag resource the caller named must stay as named";
        expect(source.out.tagResource() == &tagPool) << "the port's tag ring must come from the named resource";
        expect(edge._dataResource == &domainPool) << "the compute domain must fill the data resource nobody named";
        expect(source.out.dataResource() == &domainPool) << "the port's stream ring must come from the compute domain's resource";
    };

    "an edge naming only its data resource keeps it while the compute domain fills the tag resource"_test = [] {
        gr::ComputeRegistry::instance().register_provider("qa-edit-mixed", &qa_edit::domainResource);
        auto&                     domainPool = static_cast<qa_edit::CountingResource&>(*qa_edit::domainResource({}, nullptr));
        qa_edit::CountingResource dataPool;
        gr::Graph                 flow;
        auto&                     source = flow.emplaceBlock<qa_edit::Source>({{"compute_domain", std::string("gpu:qa-edit-mixed")}});
        auto&                     sink   = flow.emplaceBlock<qa_edit::Sink>();
        expect(flow.connect<"out", "in">(source, sink, gr::EdgeParameters{.dataResource = &dataPool}).has_value());
        expect(flow.connectPendingEdges());

        const gr::Edge& edge = flow.edges()[0];
        expect(edge._dataResource == &dataPool) << "a data resource the caller named must stay as named";
        expect(source.out.dataResource() == &dataPool) << "the port's stream ring must come from the named resource";
        expect(edge._tagResource == &domainPool) << "the compute domain must fill the tag resource nobody named";
        expect(source.out.tagResource() == &domainPool) << "the port's tag ring must come from the compute domain's resource";
    };

    "an edge naming both resources keeps both under a compute domain"_test = [] {
        gr::ComputeRegistry::instance().register_provider("qa-edit-mixed", &qa_edit::domainResource);
        qa_edit::CountingResource dataPool;
        qa_edit::CountingResource tagPool;
        gr::Graph                 flow;
        auto&                     source = flow.emplaceBlock<qa_edit::Source>({{"compute_domain", std::string("gpu:qa-edit-mixed")}});
        auto&                     sink   = flow.emplaceBlock<qa_edit::Sink>();
        expect(flow.connect<"out", "in">(source, sink, gr::EdgeParameters{.dataResource = &dataPool, .tagResource = &tagPool}).has_value());
        expect(flow.connectPendingEdges());

        const gr::Edge& edge = flow.edges()[0];
        expect(edge._dataResource == &dataPool) << "a data resource the caller named must stay as named";
        expect(edge._tagResource == &tagPool) << "a tag resource the caller named must stay as named";
        expect(source.out.dataResource() == &dataPool) << "the port's stream ring must come from the named resource";
        expect(source.out.tagResource() == &tagPool) << "the port's tag ring must come from the named resource";
    };

    "a fan-out mixing typed and dynamic connects feeds every consumer"_test = [] {
        auto runMixedFanOut = [](bool typedFirst) {
            const std::string order = typedFirst ? "typed edge first" : "dynamic edge first";

            gr::Graph flow;
            auto&     source      = flow.emplaceBlock<qa_edit::CountingSource>();
            auto&     typedSink   = flow.emplaceBlock<qa_edit::Sink>();
            auto&     dynamicSink = flow.emplaceBlock<qa_edit::Sink>();

            const auto connectTyped   = [&] { return flow.connect<"out", "in">(source, typedSink).has_value(); };
            const auto connectDynamic = [&] { return flow.connect(source, gr::PortDefinition("out"), dynamicSink, gr::PortDefinition("in")).has_value(); };

            if (typedFirst) {
                expect(connectTyped()) << order << ": the typed edge was not accepted";
                expect(connectDynamic()) << order << ": the dynamic edge was not accepted";
            } else {
                expect(connectDynamic()) << order << ": the dynamic edge was not accepted";
                expect(connectTyped()) << order << ": the typed edge was not accepted";
            }

            gr::scheduler::Simple scheduler;
            expect(fatal(scheduler.exchange(std::move(flow)).has_value()));

            std::thread runner([&scheduler] { std::ignore = scheduler.runAndWait(); });
            for (std::size_t i = 0UZ; i < 2000UZ && (typedSink._nReceived == 0UZ || dynamicSink._nReceived == 0UZ); ++i) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            std::ignore = scheduler.changeStateTo(REQUESTED_STOP);
            runner.join();

            expect(gt(typedSink._nReceived, 0UZ)) << order << ": the index-addressed consumer of the fan-out received nothing";
            expect(gt(dynamicSink._nReceived, 0UZ)) << order << ": the name-addressed consumer of the fan-out received nothing";
        };

        runMixedFanOut(true);
        runMixedFanOut(false);
    };

    "a mixed-style fan-out is one adjacency-list entry"_test = [] {
        gr::Graph flow;
        auto&     source      = flow.emplaceBlock<qa_edit::Source>();
        auto&     typedSink   = flow.emplaceBlock<qa_edit::Sink>();
        auto&     dynamicSink = flow.emplaceBlock<qa_edit::Sink>();
        expect(flow.connect<"out", "in">(source, typedSink).has_value());
        expect(flow.connect(source, gr::PortDefinition("out"), dynamicSink, gr::PortDefinition("in")).has_value());

        const gr::graph::AdjacencyList         adjacencyList = gr::graph::computeAdjacencyList(flow);
        const std::shared_ptr<gr::BlockModel>& sourceModel   = flow.blocks().front();

        expect(fatal(adjacencyList.contains(sourceModel)));
        expect(eq(adjacencyList.at(sourceModel).size(), 1UZ)) << "one output port must not occupy two entries";
        expect(eq(gr::graph::outgoingEdges(adjacencyList, sourceModel, gr::PortDefinition("out")).size(), 2UZ)) << "the name-addressed query missed an edge";
        expect(eq(gr::graph::outgoingEdges(adjacencyList, sourceModel, gr::PortDefinition(0UZ)).size(), 2UZ)) << "the index-addressed query missed an edge";
    };

    "removing one edge of a fan-out leaves the sibling flowing and stays removed"_test = [] {
        gr::Graph flow;
        auto&     source = flow.emplaceBlock<qa_edit::Source>();
        auto&     sinkA  = flow.emplaceBlock<qa_edit::Sink>();
        auto&     sinkB  = flow.emplaceBlock<qa_edit::Sink>();
        expect(flow.connect<"out", "in">(source, sinkA).has_value());
        expect(flow.connect<"out", "in">(source, sinkB).has_value());
        expect(eq(flow.edges().size(), 2UZ));

        expect(flow.connectPendingEdges()) << "the fan-out did not connect";

        const auto removed = flow.removeEdgeBySourcePort(source.unique_name, "out", sinkA.unique_name, "in");
        expect(fatal(removed.has_value())) << "the edge could not be removed: " << (removed.has_value() ? std::string{} : removed.error().message);
        expect(eq(*removed, 1UZ)) << "removing one edge of the fan-out removed a different number of edges";
        expect(eq(flow.edges().size(), 1UZ)) << "the removed edge was left in the edge list and will be resurrected on restart";
        expect(eq(flow.edges()[0].destinationBlock()->uniqueName(), std::string_view(sinkB.unique_name))) << "the wrong edge was removed";
        expect(flow.edges()[0].state() == gr::Edge::EdgeState::Connected) << "the sibling edge was left dead after the port teardown";

        // a restart must not bring the removed edge back
        flow.disconnectAllEdges();
        expect(flow.connectPendingEdges()) << "the graph did not reconnect after a restart";
        expect(eq(flow.edges().size(), 1UZ)) << "the removed edge came back on restart";

        expect(!flow.removeEdgeBySourcePort(source.unique_name, "out", sinkA.unique_name, "in").has_value()) << "removing an absent edge reported success";
    };

    "an edge removed before a first run carries nothing when the graph runs"_test = [] {
        gr::Graph flow;
        auto&     source  = flow.emplaceBlock<qa_edit::FiniteSource>();
        auto&     kept    = flow.emplaceBlock<qa_edit::Sink>();
        auto&     removed = flow.emplaceBlock<qa_edit::Sink>();
        expect(flow.connect<"out", "in">(source, kept).has_value());
        expect(flow.connect<"out", "in">(source, removed).has_value());

        const auto nRemoved = flow.removeEdgeBySourcePort(source.unique_name, "out", removed.unique_name, "in");
        expect(fatal(nRemoved.has_value())) << "the removal was refused: " << (nRemoved.has_value() ? std::string{} : nRemoved.error().message);
        expect(eq(*nRemoved, 1UZ));
        expect(fatal(eq(flow.edges().size(), 1UZ)));
        expect(flow.edges()[0].state() == gr::Edge::EdgeState::WaitingToBeConnected) << "the removal connected the remaining edge";
        expect(!source.out.isConnected()) << "the removal connected the source port";

        gr::scheduler::Simple scheduler;
        expect(fatal(scheduler.exchange(std::move(flow)).has_value()));
        expect(qa_edit::runUntil(scheduler, [&kept] { return kept.state() == gr::lifecycle::State::STOPPED; })) << "the kept consumer did not finish";
        expect(eq(kept._nReceived, qa_edit::FiniteSource::kSamples)) << "the kept consumer missed samples";
        expect(eq(removed._nReceived, 0UZ)) << "the removed edge carried samples";
    };

    "an edge removed after a run to completion carries nothing in the next run"_test = [] {
        constexpr std::size_t kSamples = qa_edit::FiniteSource::kSamples;

        gr::Graph flow;
        auto&     source  = flow.emplaceBlock<qa_edit::FiniteSource>();
        auto&     kept    = flow.emplaceBlock<qa_edit::Sink>();
        auto&     removed = flow.emplaceBlock<qa_edit::Sink>();
        expect(flow.connect<"out", "in">(source, kept).has_value());
        expect(flow.connect<"out", "in">(source, removed).has_value());

        gr::scheduler::Simple scheduler;
        expect(fatal(scheduler.exchange(std::move(flow)).has_value()));
        expect(fatal(scheduler.runAndWait().has_value()));
        expect(fatal(eq(kept._nReceived, kSamples)));
        expect(fatal(eq(removed._nReceived, kSamples)));
        expect(fatal(!source.out.isConnected())) << "the stopped consumers left the source port connected";

        const auto nRemoved = scheduler.graph().removeEdgeBySourcePort(source.unique_name, "out", removed.unique_name, "in");
        expect(fatal(nRemoved.has_value())) << "the removal was refused: " << (nRemoved.has_value() ? std::string{} : nRemoved.error().message);
        expect(eq(*nRemoved, 1UZ));
        expect(eq(scheduler.graph().edges().size(), 1UZ));

        expect(qa_edit::runUntil(scheduler, [&kept] { return kept.state() == gr::lifecycle::State::STOPPED; })) << "the kept consumer did not finish";
        expect(eq(kept._nReceived, 2UZ * kSamples)) << "the kept consumer missed samples in the second run";
        expect(eq(removed._nReceived, kSamples)) << "the removed edge carried samples in the second run";
    };

    "a recorded edge removed beside a connected sibling leaves the sibling its buffer"_test = [] {
        constexpr std::size_t kUnread = 16UZ;

        gr::Graph flow;
        auto&     source    = flow.emplaceBlock<qa_edit::Source>();
        auto&     connected = flow.emplaceBlock<qa_edit::Sink>();
        auto&     recorded  = flow.emplaceBlock<qa_edit::Sink>();
        expect(flow.connect<"out", "in">(source, connected).has_value());
        expect(fatal(flow.connectPendingEdges()));
        {
            auto written = source.out.streamWriter().reserve<gr::SpanReleasePolicy::ProcessAll>(kUnread);
            std::fill(written.begin(), written.end(), 1.0f);
        }
        expect(fatal(eq(connected.in.streamReader().available(), kUnread)));

        expect(flow.connect<"out", "in">(source, recorded).has_value());
        const auto nRemoved = flow.removeEdgeBySourcePort(source.unique_name, "out", recorded.unique_name, "in");
        expect(fatal(nRemoved.has_value())) << "the removal was refused: " << (nRemoved.has_value() ? std::string{} : nRemoved.error().message);
        expect(eq(*nRemoved, 1UZ));
        expect(fatal(eq(flow.edges().size(), 1UZ)));
        expect(eq(flow.edges()[0].destinationBlock()->uniqueName(), std::string_view(connected.unique_name))) << "the wrong edge was removed";
        expect(flow.edges()[0].state() == gr::Edge::EdgeState::Connected);
        expect(eq(connected.in.streamReader().available(), kUnread)) << "the connected sibling was given a new buffer and lost its unread samples";
        expect(!recorded.in.isConnected()) << "the removed edge was connected";
    };

    "an emplaced edge removed again releases its reader"_test = [] {
        constexpr std::size_t kWritten = 16UZ;

        gr::Graph flow;
        auto&     source = flow.emplaceBlock<qa_edit::Source>();
        auto&     kept   = flow.emplaceBlock<qa_edit::Sink>();
        auto&     added  = flow.emplaceBlock<qa_edit::Sink>();
        expect(flow.connect<"out", "in">(source, kept).has_value());
        expect(fatal(flow.connectPendingEdges()));
        const auto emplaced = flow.emplaceEdge(source.unique_name, "out", added.unique_name, "in", gr::undefined_size, 0, "added");
        expect(fatal(emplaced.has_value())) << "the edge could not be emplaced: " << (emplaced.has_value() ? std::string{} : emplaced.error().message);
        expect(fatal(added.in.isConnected())) << "the emplaced edge did not connect its consumer";
        expect(fatal(eq(source.out.nReaders(), 2UZ)));

        const auto nRemoved = flow.removeEdgeBySourcePort(source.unique_name, "out", added.unique_name, "in");
        expect(fatal(nRemoved.has_value())) << "the removal was refused: " << (nRemoved.has_value() ? std::string{} : nRemoved.error().message);
        expect(eq(*nRemoved, 1UZ));
        {
            auto written = source.out.streamWriter().reserve<gr::SpanReleasePolicy::ProcessAll>(kWritten);
            std::fill(written.begin(), written.end(), 1.0f);
        }
        expect(eq(source.out.nReaders(), 1UZ)) << "the removed edge still holds a reader of the source";
        expect(!added.in.isConnected()) << "the removed consumer is still connected";
        expect(fatal(kept.in.isConnected())) << "the kept consumer lost its connection";
        expect(eq(kept.in.streamReader().available(), kWritten)) << "the kept consumer does not see the source's samples";
    };

    "an edge recorded into a taken input and removed again leaves the source's connected sibling its buffer"_test = [] {
        constexpr std::size_t kUnread = 16UZ;

        gr::Graph flow;
        auto&     first   = flow.emplaceBlock<qa_edit::Source>({{"name", std::string("first")}});
        auto&     second  = flow.emplaceBlock<qa_edit::Source>({{"name", std::string("second")}});
        auto&     taken   = flow.emplaceBlock<qa_edit::Sink>();
        auto&     sibling = flow.emplaceBlock<qa_edit::Sink>();
        expect(flow.connect<"out", "in">(first, taken).has_value());
        expect(flow.connect<"out", "in">(second, sibling).has_value());
        expect(fatal(flow.connectPendingEdges()));
        {
            auto written = second.out.streamWriter().reserve<gr::SpanReleasePolicy::ProcessAll>(kUnread);
            std::fill(written.begin(), written.end(), 1.0f);
        }
        expect(fatal(eq(sibling.in.streamReader().available(), kUnread)));

        {
            qa_edit::StdoutCapture capture; // the replacement report
            expect(flow.connect<"out", "in">(second, taken).has_value());
        }
        expect(fatal(eq(first.out.nReaders(), 1UZ))) << "the recorded edge rewired the taken input before the next start";

        const auto nRemoved = flow.removeEdgeBySourcePort(second.unique_name, "out", taken.unique_name, "in");
        expect(fatal(nRemoved.has_value())) << "the removal was refused: " << (nRemoved.has_value() ? std::string{} : nRemoved.error().message);
        expect(eq(*nRemoved, 1UZ));
        expect(fatal(eq(flow.edges().size(), 1UZ)));
        expect(eq(flow.edges()[0].destinationBlock()->uniqueName(), std::string_view(sibling.unique_name))) << "the wrong edge was removed";
        expect(flow.edges()[0].state() == gr::Edge::EdgeState::Connected);
        expect(eq(second.out.nReaders(), 1UZ));
        expect(eq(sibling.in.streamReader().available(), kUnread)) << "the connected sibling was given a new buffer and lost its unread samples";
    };

    "a message edge removed beside a message input that reads another source releases its reader"_test = [] {
        gr::Graph flow;
        auto&     first   = flow.emplaceBlock<qa_edit::EventBlock>({{"name", std::string("first")}});
        auto&     second  = flow.emplaceBlock<qa_edit::EventBlock>({{"name", std::string("second")}});
        auto&     shared  = flow.emplaceBlock<qa_edit::EventBlock>({{"name", std::string("shared")}});
        auto&     removed = flow.emplaceBlock<qa_edit::EventBlock>({{"name", std::string("removed")}});
        expect(flow.connect<"eventsOut", "eventsIn">(first, shared).has_value());
        expect(flow.connect<"eventsOut", "eventsIn">(first, removed).has_value());
        expect(flow.connect<"eventsOut", "eventsIn">(second, shared).has_value());
        expect(fatal(flow.connectPendingEdges()));
        expect(fatal(eq(flow.edges().size(), 3UZ))) << "a message input keeps the record of each source";
        expect(fatal(eq(first.eventsOut.nReaders(), 1UZ))) << "the shared message input reads the source connected last";

        const auto nRemoved = flow.removeEdgeBySourcePort(first.unique_name, "eventsOut", removed.unique_name, "eventsIn");
        expect(fatal(nRemoved.has_value())) << "the removal was refused: " << (nRemoved.has_value() ? std::string{} : nRemoved.error().message);
        expect(eq(*nRemoved, 1UZ));
        expect(!removed.eventsIn.isConnected()) << "the removed message edge still reads its source";
    };

    "the RemoveEdge message removes an edge of a graph before its first run"_test = [] {
        using namespace gr::serialization_fields;

        qa_edit::TestScheduler scheduler;
        gr::Graph              flow;
        auto&                  source  = flow.emplaceBlock<qa_edit::Source>();
        auto&                  kept    = flow.emplaceBlock<qa_edit::Sink>();
        auto&                  removed = flow.emplaceBlock<qa_edit::Sink>();
        expect(flow.connect<"out", "in">(source, kept).has_value());
        expect(flow.connect<"out", "in">(source, removed).has_value());
        expect(fatal(scheduler.exchange(std::move(flow)).has_value()));

        gr::MsgPortOut toScheduler;
        gr::MsgPortIn  fromScheduler;
        expect(toScheduler.connect(scheduler.msgIn).has_value());
        expect(scheduler.msgOut.connect(fromScheduler).has_value());

        qa_edit::sendMessage(toScheduler, gr::scheduler::property::kRemoveEdge,
            {{std::pmr::string(EDGE_SOURCE_BLOCK), std::string(source.unique_name)}, {std::pmr::string(EDGE_SOURCE_PORT), std::string("out")}, //
                {std::pmr::string(EDGE_DESTINATION_BLOCK), std::string(removed.unique_name)}, {std::pmr::string(EDGE_DESTINATION_PORT), std::string("in")}});
        scheduler.processScheduledMessages();

        const std::optional<gr::property_map> reply = qa_edit::awaitReplyData(fromScheduler, gr::scheduler::property::kEdgeRemoved);
        expect(fatal(reply.has_value())) << "the scheduler did not report the edge removed";
        const auto* nRemoved = reply->contains(std::pmr::string("nEdgesRemoved")) ? reply->at(std::pmr::string("nEdgesRemoved")).get_if<gr::Size_t>() : nullptr;
        expect(fatal(nRemoved != nullptr)) << "the reply carries no nEdgesRemoved";
        expect(eq(*nRemoved, gr::Size_t{1})) << "the message removed a different number of edges";
        expect(fatal(eq(scheduler.graph().edges().size(), 1UZ)));
        expect(eq(scheduler.graph().edges()[0].destinationBlock()->uniqueName(), std::string_view(kept.unique_name))) << "the wrong edge was removed";
    };

#ifndef GR_TEST_WITHOUT_BLOCK_REGISTRY // emplacement by name resolves the type through the registry
    "emplacing and removing blocks while the graph runs"_test = [] {
        constexpr std::size_t kCycles = 8UZ;

        qa_edit::registerTestBlocks();

        qa_edit::TestScheduler scheduler;
        {
            gr::Graph flow;
            auto&     source = flow.emplaceBlock<qa_edit::Source>();
            auto&     sink   = flow.emplaceBlock<qa_edit::Sink>();
            expect(flow.connect<"out", "in">(source, sink).has_value());
            expect(scheduler.exchange(std::move(flow)).has_value());
        }

        gr::MsgPortOut toScheduler;
        gr::MsgPortIn  fromScheduler;
        expect(toScheduler.connect(scheduler.msgIn).has_value());
        expect(scheduler.msgOut.connect(fromScheduler).has_value());

        expect(scheduler.changeStateTo(INITIALISED).has_value());
        expect(scheduler.changeStateTo(RUNNING).has_value());

        const std::string blockYaml = std::format("id: {}\nparameters:\n  gain: !!float32 2.0\n", gr::meta::type_name<qa_edit::Tunable>());
        for (std::size_t cycle = 0UZ; cycle < kCycles; ++cycle) {
            qa_edit::sendMessage(toScheduler, gr::scheduler::property::kEmplaceBlock, {{"yaml", blockYaml}});
            expect(qa_edit::awaitReply(fromScheduler, gr::scheduler::property::kBlockEmplaced)) << "cycle " << cycle << ": block was never emplaced";

            std::string emplacedName;
            for (const auto& block : scheduler.graph().blocks()) {
                if (block->typeName().find("Tunable") != std::string_view::npos) {
                    emplacedName = block->uniqueName();
                }
            }
            expect(!emplacedName.empty()) << "cycle " << cycle << ": the emplaced block is not in the graph";

            qa_edit::sendMessage(toScheduler, gr::scheduler::property::kRemoveBlock, {{"uniqueName", emplacedName}});
            expect(qa_edit::awaitReply(fromScheduler, gr::scheduler::property::kBlockRemoved)) << "cycle " << cycle << ": block was never removed";
        }

        expect(scheduler.changeStateTo(REQUESTED_STOP).has_value());
        for (std::size_t i = 0UZ; i < 3000UZ && scheduler.state() != STOPPED; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        expect(scheduler.state() == STOPPED) << "the scheduler did not stop after the edit cycles";
    };

#endif

    // the fields of an edge message come from whoever sent it, so a value of the wrong type is
    // unusable input: the message is answered as incomplete rather than ending the process, which
    // is what a terminating pointer read of the buffer size or the weight would do
    "an edge message whose weight is of the wrong type is refused"_test = [] {
        using namespace gr::serialization_fields;

        qa_edit::TestScheduler scheduler;
        {
            gr::Graph flow;
            auto&     source = flow.emplaceBlock<qa_edit::Source>();
            auto&     sink   = flow.emplaceBlock<qa_edit::Sink>();
            expect(flow.connect<"out", "in">(source, sink).has_value());
            expect(scheduler.exchange(std::move(flow)).has_value());
        }

        gr::MsgPortOut toScheduler;
        gr::MsgPortIn  fromScheduler;
        expect(toScheduler.connect(scheduler.msgIn).has_value());
        expect(scheduler.msgOut.connect(fromScheduler).has_value());

        qa_edit::sendMessage(toScheduler, gr::scheduler::property::kEmplaceEdge,
            {{std::pmr::string(EDGE_SOURCE_BLOCK), std::string("source")}, {std::pmr::string(EDGE_SOURCE_PORT), std::string("out")},           //
                {std::pmr::string(EDGE_DESTINATION_BLOCK), std::string("sink")}, {std::pmr::string(EDGE_DESTINATION_PORT), std::string("in")}, //
                {std::pmr::string(EDGE_MIN_BUFFER_SIZE), gr::undefined_Size}, {std::pmr::string(EDGE_WEIGHT), std::string("heavy")},           //
                {std::pmr::string(EDGE_NAME), std::string("wrong weight")}});

        expect(scheduler.changeStateTo(INITIALISED).has_value());
        expect(scheduler.changeStateTo(RUNNING).has_value());

        const std::string reported = qa_edit::awaitError(fromScheduler, gr::scheduler::property::kEdgeEmplaced);
        expect(!reported.empty()) << "a weight of the wrong type must be reported, not end the process";

        expect(scheduler.changeStateTo(REQUESTED_STOP).has_value());
    };

    // the same shape on the subgraph export path, and reached without a scheduler because the
    // handler answers the message the wrapper processes
    "a subgraph export message whose exportFlag is of the wrong type is refused"_test = [] {
        gr::GraphWrapper<gr::Graph> subgraph;

        gr::MsgPortOut toSubgraph;
        gr::MsgPortIn  fromSubgraph;
        expect(toSubgraph.connect(*subgraph.msgIn).has_value());
        expect(subgraph.msgOut->connect(fromSubgraph).has_value());

        qa_edit::sendMessage(toSubgraph, gr::graph::property::kSubgraphExportPort,
            {{"uniqueBlockName", std::string("inner")}, {"portDirection", std::string("output")}, //
                {"portName", std::string("out")}, {"exportFlag", std::string("yes")}});

        subgraph.processScheduledMessages();

        bool refused = false;
        auto replies = fromSubgraph.streamReader().get();
        for (const gr::Message& reply : replies) {
            refused = refused || !reply.data.has_value();
        }
        std::ignore = replies.consume(replies.size());
        expect(refused) << "an exportFlag that is not a bool must be reported, not end the process";
    };

    "an edge sized by duration follows the sample rate"_test = [] {
        using gr::graph::edgeBufferSizeFor;
        using gr::graph::kDefaultEdgeBufferSeconds;
        using gr::graph::kMaxEdgeBufferSize;
        using gr::graph::kMinEdgeBufferSize;

        constexpr double rates[] = {8.0e3, 48.0e3, 96.0e3, 240.0e3, 2.4e6, 25.0e6, 61.44e6};
        for (const double rate : rates) {
            const std::size_t nSamples = edgeBufferSizeFor(rate);
            expect(eq(nSamples, std::bit_ceil(nSamples))) << rate << ": the ring is not a power of two";
            expect(ge(nSamples, kMinEdgeBufferSize)) << rate << ": the ring is below the floor";
            expect(le(nSamples, kMaxEdgeBufferSize)) << rate << ": the ring is above the ceiling";
            if (nSamples > kMinEdgeBufferSize && nSamples < kMaxEdgeBufferSize) {
                expect(ge(static_cast<double>(nSamples) / rate, kDefaultEdgeBufferSeconds)) << rate << ": the ring holds less than the stated duration";
                expect(lt(static_cast<double>(nSamples) / rate, 2.0 * kDefaultEdgeBufferSeconds)) << rate << ": rounding is the only excess allowed";
            }
        }

        expect(gt(edgeBufferSizeFor(25.0e6), edgeBufferSizeFor(2.4e6))) << "ten times the rate must not give the same ring, which is what a fixed count does";
        expect(eq(edgeBufferSizeFor(48.0e3), kMinEdgeBufferSize)) << "a rate too low to fill the smallest ring must get the smallest ring";
        expect(eq(edgeBufferSizeFor(61.44e6), kMaxEdgeBufferSize)) << "a rate asking for more than the ceiling must be held at it";
        expect(eq(edgeBufferSizeFor(0.0), kMinEdgeBufferSize)) << "an unknown rate must get the smallest ring, not an empty one";
        expect(eq(edgeBufferSizeFor(-1.0), kMinEdgeBufferSize)) << "a negative rate must get the smallest ring, not an empty one";
        expect(eq(edgeBufferSizeFor(1.0e12), kMaxEdgeBufferSize)) << "a rate asking for hundreds of megabytes must be held at the ceiling";
        expect(eq(edgeBufferSizeFor(2.4e6, 0.001), kMinEdgeBufferSize)) << "a shorter duration must reach the floor at a rate the default does not";

        expect(eq(kMinEdgeBufferSize, gr::PortOut<float>::kDefaultBufferSize)) << "the floor is the default size of a port's buffer";
        expect(eq(edgeBufferSizeFor(8.0e3), kMinEdgeBufferSize)) << "8 kHz: 400 samples, raised to the floor";
        expect(eq(edgeBufferSizeFor(48.0e3), kMinEdgeBufferSize)) << "48 kHz: 2400 samples, raised to the floor";
        expect(eq(edgeBufferSizeFor(96.0e3), 8192UZ)) << "96 kHz: 4800 rounded up";
        expect(eq(edgeBufferSizeFor(240.0e3), 16384UZ)) << "240 kHz: 12000 rounded up";
        expect(eq(edgeBufferSizeFor(81920.0), kMinEdgeBufferSize)) << "the lower knee is the floor over the seconds, and the floor still answers it";
        expect(eq(edgeBufferSizeFor(81940.0), 2UZ * kMinEdgeBufferSize)) << "one sample past the knee the rate takes over";
        expect(eq(edgeBufferSizeFor(83886080.0), kMaxEdgeBufferSize)) << "the upper knee is the ceiling over the seconds";
    };

    "a second edge into a taken stream input replaces the first"_test = [] {
        using namespace gr::serialization_fields;

        gr::Graph flow;
        auto&     first  = flow.emplaceBlock<qa_edit::Source>({{"name", std::string("first")}});
        auto&     second = flow.emplaceBlock<qa_edit::Source>({{"name", std::string("second")}});
        auto&     sink   = flow.emplaceBlock<qa_edit::Sink>();
        expect(flow.connect<"out", "in">(first, sink).has_value());

        bool        connected = false;
        std::string reported;
        {
            qa_edit::StdoutCapture capture;
            // the typed connect names the input by index and this one names it by name
            connected = flow.connect(second, gr::PortDefinition("out"), sink, gr::PortDefinition("in")).has_value();
            reported  = capture.text();
        }
        expect(connected);

        expect(fatal(eq(flow.edges().size(), 1UZ))) << "the displaced edge stayed listed";
        expect(eq(flow.edges()[0].sourceBlock()->uniqueName(), std::string_view(second.unique_name))) << "the earlier edge was kept instead of the later one";
        expect(reported.contains(qa_edit::portOf("first", "0")) && reported.contains(qa_edit::portOf("second", "out"))) << "the replacement must name both edges, reported: " << reported;

        expect(flow.connectPendingEdges());
        expect(flow.edges()[0].state() == gr::Edge::EdgeState::Connected);
        expect(eq(first.out.nReaders(), 0UZ)) << "the displaced source still feeds the input";
        expect(eq(second.out.nReaders(), 1UZ)) << "the listed edge carries no data";

        const std::optional<gr::Message> inspected = flow.propertyCallbackGraphInspect(gr::graph::property::kGraphInspect, {});
        expect(fatal(inspected.has_value() && inspected->data.has_value()));
        const auto* inspectedEdges = inspected->data->at(std::pmr::string(BLOCK_EDGES)).get_if<gr::property_map>();
        expect(fatal(inspectedEdges != nullptr));
        expect(fatal(eq(inspectedEdges->size(), 1UZ))) << "the inspect reply lists an edge the graph does not carry";
        const auto* inspectedEdge = inspectedEdges->begin()->second.get_if<gr::property_map>();
        expect(fatal(inspectedEdge != nullptr));
        expect(eq(inspectedEdge->at(std::pmr::string(EDGE_SOURCE_BLOCK)).value_or(std::string_view{}), std::string_view(second.unique_name))) << "the inspect reply names the displaced source";
    };

    "an edge emplaced into a taken stream input replaces the listed edge"_test = [] {
        using namespace gr::serialization_fields;

        qa_edit::TestScheduler scheduler;
        gr::Graph              flow;
        auto&                  first  = flow.emplaceBlock<qa_edit::Source>({{"name", std::string("first")}});
        auto&                  second = flow.emplaceBlock<qa_edit::Source>({{"name", std::string("second")}});
        auto&                  sink   = flow.emplaceBlock<qa_edit::Sink>();
        expect(flow.connect<"out", "in">(first, sink).has_value());
        expect(fatal(scheduler.exchange(std::move(flow)).has_value()));

        gr::MsgPortOut toScheduler;
        gr::MsgPortIn  fromScheduler;
        expect(toScheduler.connect(scheduler.msgIn).has_value());
        expect(scheduler.msgOut.connect(fromScheduler).has_value());

        expect(scheduler.changeStateTo(INITIALISED).has_value());
        expect(scheduler.changeStateTo(RUNNING).has_value());

        std::optional<gr::property_map> reply;
        std::string                     reported;
        {
            qa_edit::StdoutCapture capture;
            qa_edit::sendMessage(toScheduler, gr::scheduler::property::kEmplaceEdge,
                {{std::pmr::string(EDGE_SOURCE_BLOCK), std::string(second.unique_name)}, {std::pmr::string(EDGE_SOURCE_PORT), std::string("out")},           //
                    {std::pmr::string(EDGE_DESTINATION_BLOCK), std::string(sink.unique_name)}, {std::pmr::string(EDGE_DESTINATION_PORT), std::string("in")}, //
                    {std::pmr::string(EDGE_MIN_BUFFER_SIZE), gr::undefined_Size}, {std::pmr::string(EDGE_WEIGHT), std::int32_t{0}},                          //
                    {std::pmr::string(EDGE_NAME), std::string("replacement")}});
            reply    = qa_edit::awaitReplyData(fromScheduler, gr::scheduler::property::kEdgeEmplaced);
            reported = capture.text();
        }
        expect(fatal(reply.has_value())) << "the edge was never emplaced";

        const std::span<const gr::Edge> edges = scheduler.graph().edges();
        expect(fatal(eq(edges.size(), 1UZ))) << "the displaced edge stayed listed";
        expect(eq(edges[0].sourceBlock()->uniqueName(), std::string_view(second.unique_name))) << "the earlier edge was kept instead of the emplaced one";
        expect(reported.contains(qa_edit::portOf("first", "0")) && reported.contains(qa_edit::portOf("second", "out"))) << "the replacement must name both edges, reported: " << reported;

        const auto* displaced = reply->contains(std::pmr::string("displacedEdges")) ? reply->at(std::pmr::string("displacedEdges")).get_if<gr::property_map>() : nullptr;
        expect(fatal(displaced != nullptr)) << "the reply does not list the displaced edges";
        expect(fatal(eq(displaced->size(), 1UZ))) << "the reply lists a different number of displaced edges";
        const auto* displacedEdge = displaced->begin()->second.get_if<gr::property_map>();
        expect(fatal(displacedEdge != nullptr));
        expect(eq(displacedEdge->at(std::pmr::string(EDGE_SOURCE_BLOCK)).value_or(std::string_view{}), std::string_view(first.unique_name))) << "the reply names another edge as displaced";
        expect(eq(displacedEdge->at(std::pmr::string(EDGE_DESTINATION_BLOCK)).value_or(std::string_view{}), std::string_view(sink.unique_name)));
        expect(eq(first.out.nReaders(), 0UZ)) << "the displaced source still feeds the input";
        expect(eq(second.out.nReaders(), 1UZ)) << "the emplaced source does not feed the input";

        expect(scheduler.changeStateTo(REQUESTED_STOP).has_value());
        for (std::size_t i = 0UZ; i < 3000UZ && scheduler.state() != STOPPED; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        expect(scheduler.state() == STOPPED);
    };

    // the functions the RemoveEdge and EmplaceEdge handlers call, in the order a rewire takes
    "an input freed by removing its edge takes a new edge without a replacement"_test = [] {
        gr::Graph flow;
        auto&     first  = flow.emplaceBlock<qa_edit::Source>({{"name", std::string("first")}});
        auto&     second = flow.emplaceBlock<qa_edit::Source>({{"name", std::string("second")}});
        auto&     sink   = flow.emplaceBlock<qa_edit::Sink>();
        expect(flow.connect<"out", "in">(first, sink).has_value());
        expect(flow.connectPendingEdges());

        const auto removed = flow.removeEdgeBySourcePort(first.unique_name, "out", sink.unique_name, "in");
        expect(fatal(removed.has_value())) << "the edge could not be removed: " << (removed.has_value() ? std::string{} : removed.error().message);

        std::expected<std::vector<gr::Edge>, gr::Error> emplaced;
        std::string                                     reported;
        {
            qa_edit::StdoutCapture capture;
            emplaced = flow.emplaceEdge(second.unique_name, "out", sink.unique_name, "in", gr::undefined_size, 0, "rewired");
            reported = capture.text();
        }
        expect(fatal(emplaced.has_value())) << "the edge could not be emplaced: " << (emplaced.has_value() ? std::string{} : emplaced.error().message);
        expect(emplaced->empty()) << "a free input returned a displaced edge";

        expect(fatal(eq(flow.edges().size(), 1UZ))) << "the rewire left a different number of edges";
        expect(eq(flow.edges()[0].sourceBlock()->uniqueName(), std::string_view(second.unique_name))) << "the rewired edge is not the one listed";
        expect(!reported.contains(qa_edit::portOf("second", "out"))) << "a free input reported a replacement: " << reported;
        expect(eq(first.out.nReaders(), 0UZ)) << "the removed source still feeds the input";
        expect(eq(second.out.nReaders(), 1UZ)) << "the rewired source does not feed the input";
    };
};

namespace qa_edit {
// records the size and alignment of each allocation made through it while it is the default resource
struct SizeRecordingResource : std::pmr::memory_resource {
    std::vector<std::pair<std::size_t, std::size_t>> allocations; // bytes, alignment
    std::pmr::memory_resource*                       previous = std::pmr::set_default_resource(this);

    SizeRecordingResource() { allocations.reserve(1024UZ); }
    ~SizeRecordingResource() override { restore(); }

    void restore() { std::pmr::set_default_resource(previous); }

    void* do_allocate(std::size_t bytes, std::size_t alignment) override {
        allocations.emplace_back(bytes, alignment);
        return std::pmr::new_delete_resource()->allocate(bytes, alignment);
    }
    void do_deallocate(void* pointer, std::size_t bytes, std::size_t alignment) override { std::pmr::new_delete_resource()->deallocate(pointer, bytes, alignment); }
    bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override { return this == &other; }

    // the heap rings a port of the default size holds: the message ring and the tag ring of either direction.
    // Each ring is matched on its byte size and on its element's alignment, which the allocator passes. Size alone is
    // not enough: under libc++ a default tag ring has the byte size of a heap float ring of 65536 samples.
    [[nodiscard]] std::size_t nDefaultRings() const {
        constexpr std::size_t                     kSlots   = gr::MsgPortOut::kDefaultBufferSize;
        const std::pair<std::size_t, std::size_t> rings[3] = {{2UZ * kSlots * sizeof(gr::Message), alignof(gr::Message)}, {2UZ * kSlots * sizeof(gr::Tag), alignof(gr::Tag)}, {2UZ * (kSlots + 1UZ) * sizeof(gr::Tag), alignof(gr::Tag)}};
        return static_cast<std::size_t>(std::ranges::count_if(allocations, [&rings](const std::pair<std::size_t, std::size_t>& allocation) { return std::ranges::find(rings, allocation) != std::ranges::end(rings); }));
    }
};
} // namespace qa_edit

const boost::ut::suite<"graph port rings"> graphPortRingTests = [] {
    using namespace boost::ut;

    "a graph builds no default ring for a port it sizes"_test = [] {
        qa_edit::SizeRecordingResource recorder;
        std::size_t                    edgeSize = 0UZ;
        {
            gr::Graph flow;
            auto&     source = flow.emplaceBlock<qa_edit::DualSource>();
            auto&     sink   = flow.emplaceBlock<qa_edit::Sink>();
            expect(fatal(flow.connect<"out", "in">(source, sink).has_value()));
            expect(fatal(flow.connectPendingEdges()));
            edgeSize = source.out.bufferSize();
            expect(eq(sink.in.bufferSize(), edgeSize)) << "the input reads the edge's ring";
            expect(!source.monitor.isConnected());
        }
        recorder.restore();
        expect(neq(edgeSize, gr::PortOut<float>::kDefaultBufferSize)) << "the edge must differ from the default size for the count to tell them apart";
        expect(eq(recorder.nDefaultRings(), 0UZ)) << std::format("building and destroying a graph built {} heap rings of the default size", recorder.nDefaultRings());
    };
};

const boost::ut::suite<"messages after a job list ends"> laterMessageTests = [] {
    using namespace boost::ut;
    using enum gr::lifecycle::State;

    // under multiThreaded each job list has a worker of its own, and a worker leaves once every block of its job list
    // is done. Here the first job list ends at once and the second runs until the stop.
    "the scheduler replies after its first job list has ended"_test = [] {
        constexpr std::string_view kPoolName = "qa_edit_two_threads";
        gr::thread_pool::Manager::instance().replacePool(std::string(kPoolName), std::make_shared<gr::thread_pool::ThreadPoolWrapper>(std::make_unique<gr::thread_pool::BasicThreadPool>(kPoolName, gr::thread_pool::TaskType::CPU_BOUND, 2U, 2U), "CPU"));

        gr::Graph flow;
        auto&     ending     = flow.emplaceBlock<qa_edit::EndingSource>();
        auto&     endingSink = flow.emplaceBlock<qa_edit::Sink>();
        auto&     source     = flow.emplaceBlock<qa_edit::Source>();
        auto&     sink       = flow.emplaceBlock<qa_edit::Sink>();
        expect(flow.connect<"out", "in">(ending, endingSink).has_value());
        expect(flow.connect<"out", "in">(source, sink).has_value());
        const std::string endingName{ending.unique_name};

        qa_edit::JobListProbe scheduler({{"poolName", std::string(kPoolName)}});
        expect(scheduler.exchange(std::move(flow)).has_value());

        gr::MsgPortOut toScheduler;
        gr::MsgPortIn  fromScheduler;
        expect(toScheduler.connect(scheduler.msgIn).has_value());
        expect(scheduler.msgOut.connect(fromScheduler).has_value());

        qa_edit::sendMessage(toScheduler, gr::scheduler::property::kSchedulerInspect, {});
        expect(scheduler.changeStateTo(INITIALISED).has_value());
        expect(scheduler.changeStateTo(RUNNING).has_value());
        expect(qa_edit::takeReply(fromScheduler, gr::scheduler::property::kSchedulerInspected)) << "the request sent before the run got no reply";

        expect(fatal(eq(scheduler.nJobLists(), 2UZ))) << "the graph was not split into two job lists";
        expect(fatal(scheduler.firstJobListHolds(endingName))) << "the ending source is not in the first job list";
        expect(fatal(qa_edit::awaitCondition([&scheduler] { return scheduler.nRunningJobs() == 1UZ; }))) << "the first job list did not end";

        qa_edit::sendMessage(toScheduler, gr::scheduler::property::kSchedulerInspect, {});
        expect(qa_edit::takeReply(fromScheduler, gr::scheduler::property::kSchedulerInspected)) << "the request sent after the first job list ended got no reply";

        expect(scheduler.changeStateTo(REQUESTED_STOP).has_value());
        expect(qa_edit::awaitCondition([&scheduler] { return scheduler.state() == STOPPED; })) << "the scheduler did not stop";
    };

    // no reader is connected to msgOut, so a child's error ends the run. The first job list ends at once, and the
    // source in the second one reports its error later
    "a child's error after the first job list has ended fails runAndWait()"_test = [] {
        constexpr std::string_view kPoolName = "qa_edit_two_threads";
        gr::thread_pool::Manager::instance().replacePool(std::string(kPoolName), std::make_shared<gr::thread_pool::ThreadPoolWrapper>(std::make_unique<gr::thread_pool::BasicThreadPool>(kPoolName, gr::thread_pool::TaskType::CPU_BOUND, 2U, 2U), "CPU"));

        gr::Graph flow;
        auto&     ending      = flow.emplaceBlock<qa_edit::EndingSource>();
        auto&     endingSink  = flow.emplaceBlock<qa_edit::Sink>();
        auto&     failing     = flow.emplaceBlock<qa_edit::LateErrorSource>();
        auto&     failingSink = flow.emplaceBlock<qa_edit::Sink>();
        expect(flow.connect<"out", "in">(ending, endingSink).has_value());
        expect(flow.connect<"out", "in">(failing, failingSink).has_value());
        const std::string endingName{ending.unique_name};
        const std::string failingName{failing.unique_name};
        const auto*       failingBlock = &failing;

        qa_edit::JobListProbe scheduler({{"poolName", std::string(kPoolName)}});
        expect(scheduler.exchange(std::move(flow)).has_value());

        const std::expected<void, gr::Error> result = scheduler.runAndWait();

        expect(fatal(eq(scheduler.nJobLists(), 2UZ))) << "the graph was not split into two job lists";
        expect(fatal(scheduler.firstJobListHolds(endingName))) << "the ending source is not in the first job list";
        expect(fatal(!result.has_value())) << "runAndWait() reported success";
        expect(result.error().message.find(failingName) != std::string::npos) << "the error must name the block: " << result.error().message;
        expect(result.error().message.find(qa_edit::kLateFault) != std::string::npos) << "the error must carry the block's reason: " << result.error().message;
        expect(scheduler.state() == ERROR) << "the run did not end in ERROR";
        expect(lt(failingBlock->_nCalls, qa_edit::LateErrorSource::kCallsToEnd)) << "the run went on to the source's own end";
    };

#ifndef GR_TEST_WITHOUT_BLOCK_REGISTRY // emplacement by name resolves the type through the registry
    "a block added after the first job list has ended runs"_test = [] {
        constexpr std::size_t      kBlocks   = 4UZ;
        constexpr std::string_view kPoolName = "qa_edit_two_threads";
        gr::thread_pool::Manager::instance().replacePool(std::string(kPoolName), std::make_shared<gr::thread_pool::ThreadPoolWrapper>(std::make_unique<gr::thread_pool::BasicThreadPool>(kPoolName, gr::thread_pool::TaskType::CPU_BOUND, 2U, 2U), "CPU"));
        qa_edit::registerTestBlocks();
        qa_edit::gWorkingTickers.store(0UZ);

        gr::Graph flow;
        auto&     ending     = flow.emplaceBlock<qa_edit::EndingSource>();
        auto&     endingSink = flow.emplaceBlock<qa_edit::Sink>();
        auto&     source     = flow.emplaceBlock<qa_edit::Source>();
        auto&     sink       = flow.emplaceBlock<qa_edit::Sink>();
        expect(flow.connect<"out", "in">(ending, endingSink).has_value());
        expect(flow.connect<"out", "in">(source, sink).has_value());
        const std::string endingName{ending.unique_name};

        qa_edit::JobListProbe scheduler({{"poolName", std::string(kPoolName)}});
        expect(scheduler.exchange(std::move(flow)).has_value());

        gr::MsgPortOut toScheduler;
        gr::MsgPortIn  fromScheduler;
        expect(toScheduler.connect(scheduler.msgIn).has_value());
        expect(scheduler.msgOut.connect(fromScheduler).has_value());

        expect(scheduler.changeStateTo(INITIALISED).has_value());
        expect(scheduler.changeStateTo(RUNNING).has_value());
        expect(fatal(eq(scheduler.nJobLists(), 2UZ))) << "the graph was not split into two job lists";
        expect(fatal(scheduler.firstJobListHolds(endingName))) << "the ending source is not in the first job list";
        expect(fatal(qa_edit::awaitCondition([&scheduler] { return scheduler.nRunningJobs() == 1UZ; }))) << "the first job list did not end";

        for (std::size_t i = 0UZ; i < kBlocks; ++i) {
            qa_edit::sendMessage(toScheduler, gr::scheduler::property::kEmplaceBlock, {{"type", gr::meta::type_name<qa_edit::Ticker>()}});
            expect(qa_edit::takeReply(fromScheduler, gr::scheduler::property::kBlockEmplaced)) << std::format("block {} was never emplaced", i);
        }
        expect(qa_edit::awaitCondition([] { return qa_edit::gWorkingTickers.load() == kBlocks; })) << std::format("{} of {} added blocks ran", qa_edit::gWorkingTickers.load(), kBlocks);

        expect(scheduler.changeStateTo(REQUESTED_STOP).has_value());
        expect(qa_edit::awaitCondition([&scheduler] { return scheduler.state() == STOPPED; })) << "the scheduler did not stop";
    };
#endif
};

#ifndef GR_TEST_WITHOUT_BLOCK_REGISTRY // emplacement and replacement by name resolve the type through the registry
const boost::ut::suite<"edit replies"> editReplyTests = [] {
    using namespace boost::ut;
    using enum gr::lifecycle::State;
    using namespace gr::scheduler::property;
    using qa_edit::EditRequest;

    "an EmplaceBlock request is answered under BlockEmplaced with its id"_test = [] {
        qa_edit::registerTestBlocks();
        qa_edit::TestScheduler scheduler;
        std::ignore = qa_edit::exchangeChain(scheduler);
        const std::string tunable{gr::meta::type_name<qa_edit::Tunable>()};

        const std::array requests{EditRequest{"emplaced", kEmplaceBlock, {{"type", tunable}}, true}, //
            EditRequest{"empty type", kEmplaceBlock, {{"type", std::string()}}},                     //
            EditRequest{"no type", kEmplaceBlock, {}},                                               //
            EditRequest{"unknown type", kEmplaceBlock, {{"type", std::string("qa_edit::Absent")}}},  //
            EditRequest{"empty yaml", kEmplaceBlock, {{"yaml", std::string()}}},                     //
            EditRequest{"no target graph", kEmplaceBlock, {{"type", tunable}, {"_targetGraph", std::string("absent")}}}};
        qa_edit::expectRepliesUnder(scheduler, kBlockEmplaced, requests);
    };

    "a RemoveBlock request is answered under BlockRemoved with its id"_test = [] {
        qa_edit::registerTestBlocks();
        qa_edit::TestScheduler    scheduler;
        const qa_edit::ChainNames chain = qa_edit::exchangeChain(scheduler);

        const std::array requests{EditRequest{"empty name", kRemoveBlock, {{"uniqueName", std::string()}}}, //
            EditRequest{"no name", kRemoveBlock, {}},                                                       //
            EditRequest{"absent block", kRemoveBlock, {{"uniqueName", std::string("absent")}}},             //
            EditRequest{"removed", kRemoveBlock, {{"uniqueName", chain.spareSink}}, true}};
        qa_edit::expectRepliesUnder(scheduler, kBlockRemoved, requests);
    };

    "a RemoveEdge request is answered under EdgeRemoved with its id"_test = [] {
        using namespace gr::serialization_fields;
        qa_edit::registerTestBlocks();
        qa_edit::TestScheduler    scheduler;
        const qa_edit::ChainNames chain = qa_edit::exchangeChain(scheduler);
        expect(fatal(scheduler.graph().connectPendingEdges())) << "the chain did not connect"; // an edge is removed from its connected ports

        const std::array requests{EditRequest{"empty source", kRemoveEdge, {{std::pmr::string(EDGE_SOURCE_BLOCK), std::string()}, {std::pmr::string(EDGE_SOURCE_PORT), std::string()}}}, //
            EditRequest{"no source", kRemoveEdge, {}},                                                                                                                                   //
            EditRequest{"absent edge", kRemoveEdge, {{std::pmr::string(EDGE_SOURCE_BLOCK), chain.source}, {std::pmr::string(EDGE_SOURCE_PORT), std::string("out")}, {std::pmr::string(EDGE_DESTINATION_BLOCK), std::string("absent")}, {std::pmr::string(EDGE_DESTINATION_PORT), std::string("in")}}}, EditRequest{"removed", kRemoveEdge, {{std::pmr::string(EDGE_SOURCE_BLOCK), chain.source}, {std::pmr::string(EDGE_SOURCE_PORT), std::string("out")}}, true}};
        qa_edit::expectRepliesUnder(scheduler, kEdgeRemoved, requests);
    };

    "an EmplaceEdge request is answered under EdgeEmplaced with its id"_test = [] {
        qa_edit::registerTestBlocks();
        qa_edit::TestScheduler    scheduler;
        const qa_edit::ChainNames chain = qa_edit::exchangeChain(scheduler);

        const std::array requests{EditRequest{"incomplete", kEmplaceEdge, {}},                                                    //
            EditRequest{"wrong weight", kEmplaceEdge, qa_edit::edgeRequest(chain.source, chain.spareSink, std::string("heavy"))}, //
            EditRequest{"absent source", kEmplaceEdge, qa_edit::edgeRequest("absent", chain.spareSink, std::int32_t{0})},         //
            EditRequest{"emplaced", kEmplaceEdge, qa_edit::edgeRequest(chain.source, chain.spareSink, std::int32_t{0}), true}};
        qa_edit::expectRepliesUnder(scheduler, kEdgeEmplaced, requests);
    };

    "a ReplaceBlock request is answered under BlockReplaced with its id"_test = [] {
        qa_edit::registerTestBlocks();
        qa_edit::TestScheduler    scheduler;
        const qa_edit::ChainNames chain = qa_edit::exchangeChain(scheduler);
        const std::string         tunable{gr::meta::type_name<qa_edit::Tunable>()};

        const std::array requests{EditRequest{"empty name", kReplaceBlock, {{"uniqueName", std::string()}, {"type", tunable}}}, //
            EditRequest{"no type", kReplaceBlock, {{"uniqueName", chain.stage}}},                                               //
            EditRequest{"absent block", kReplaceBlock, {{"uniqueName", std::string("absent")}, {"type", tunable}}},             //
            EditRequest{"replaced", kReplaceBlock, {{"uniqueName", chain.stage}, {"type", tunable}}, true}};
        qa_edit::expectRepliesUnder(scheduler, kBlockReplaced, requests);
    };

    "a GraphGRC request is answered under GraphGRC with its id"_test = [] {
        qa_edit::registerTestBlocks();
        qa_edit::TestScheduler scheduler;
        std::ignore = qa_edit::exchangeChain(scheduler);

        const std::array requests{EditRequest{"read", kGraphGRC, {}, true, gr::message::Command::Get}, //
            EditRequest{"empty yaml", kGraphGRC, {{"value", std::string()}}},                          //
            EditRequest{"no yaml", kGraphGRC, {}},                                                     //
            EditRequest{"exchanged", kGraphGRC, {{"value", gr::saveGrc(gr::globalPluginLoader(), gr::Graph{})}}, true}};
        qa_edit::expectRepliesUnder(scheduler, kGraphGRC, requests);
    };

    // the run's two job lists end at once, and the scheduler stays RUNNING with no worker left to take the block
    "a block emplaced after every worker of the run has left is answered with the reason"_test = [] {
        constexpr std::string_view kPoolName = "qa_edit_two_threads";
        gr::thread_pool::Manager::instance().replacePool(std::string(kPoolName), std::make_shared<gr::thread_pool::ThreadPoolWrapper>(std::make_unique<gr::thread_pool::BasicThreadPool>(kPoolName, gr::thread_pool::TaskType::CPU_BOUND, 2U, 2U), "CPU"));
        qa_edit::registerTestBlocks();
        qa_edit::gWorkingTickers.store(0UZ);

        gr::Graph flow;
        auto&     ending     = flow.emplaceBlock<qa_edit::EndingSource>();
        auto&     endingSink = flow.emplaceBlock<qa_edit::Sink>();
        expect(flow.connect<"out", "in">(ending, endingSink).has_value());

        qa_edit::JobListProbe scheduler({{"poolName", std::string(kPoolName)}});
        expect(scheduler.exchange(std::move(flow)).has_value());

        gr::MsgPortOut toScheduler;
        gr::MsgPortIn  fromScheduler;
        expect(toScheduler.connect(scheduler.msgIn).has_value());
        expect(scheduler.msgOut.connect(fromScheduler).has_value());

        expect(scheduler.changeStateTo(INITIALISED).has_value());
        expect(scheduler.changeStateTo(RUNNING).has_value());
        expect(fatal(qa_edit::awaitCondition([&scheduler] { return scheduler.nRunningJobs() == 0UZ; }))) << "the run's workers did not leave";
        expect(fatal(scheduler.state() == RUNNING));

        gr::sendMessage<gr::message::Command::Set>(toScheduler, scheduler.unique_name, kEmplaceBlock, {{"type", gr::meta::type_name<qa_edit::Ticker>()}}, "late");
        scheduler.processScheduledMessages();
        const std::optional<gr::Message> reply = qa_edit::takeReplyTo(fromScheduler, "late");
        expect(fatal(reply.has_value())) << "the request got no reply with its id";
        expect(eq(reply->endpoint, std::string(kBlockEmplaced)));
        expect(fatal(!reply->data.has_value())) << "the reply says the block joined a run that has no worker";
        expect(reply->data.error().message.contains("next start")) << "the reply does not say when the block runs: " << reply->data.error().message;
        expect(eq(qa_edit::gWorkingTickers.load(), 0UZ));

        expect(scheduler.changeStateTo(REQUESTED_STOP).has_value());
        expect(qa_edit::awaitCondition([&scheduler] { return scheduler.state() == STOPPED; })) << "the scheduler did not stop";
    };
};

const boost::ut::suite<"a replacement in a running graph"> runningReplacementTests = [] {
    using namespace boost::ut;
    using enum gr::lifecycle::State;
    using namespace gr::scheduler::property;

    "a replacement in a multi-threaded run passes on the samples queued at its ports"_test = [] { qa_edit::expectQueuedSamplesPassReplacement<gr::scheduler::Simple<gr::scheduler::ExecutionPolicy::multiThreaded>>(); };

    "a replacement in a single-threaded run passes on the samples queued at its ports"_test = [] { qa_edit::expectQueuedSamplesPassReplacement<gr::scheduler::Simple<gr::scheduler::ExecutionPolicy::singleThreaded>>(); };

    "a replacement in a single-threaded blocking run passes on the samples queued at its ports"_test = [] { qa_edit::expectQueuedSamplesPassReplacement<gr::scheduler::Simple<gr::scheduler::ExecutionPolicy::singleThreadedBlocking>>(); };

    // a pool of three threads splits the three blocks into three job lists
    "a replacement joins the job list of the block it replaces, and the reply names that job list"_test = [] {
        constexpr std::string_view kPoolName = "qa_edit_three_threads";
        gr::thread_pool::Manager::instance().replacePool(std::string(kPoolName), std::make_shared<gr::thread_pool::ThreadPoolWrapper>(std::make_unique<gr::thread_pool::BasicThreadPool>(kPoolName, gr::thread_pool::TaskType::CPU_BOUND, 3U, 3U), "CPU"));
        qa_edit::registerTestBlocks();
        qa_edit::gCountedSamples.store(0UZ);

        gr::Graph flow;
        auto&     source = flow.emplaceBlock<qa_edit::Source>();
        auto&     stage  = flow.emplaceBlock<qa_edit::Tunable>();
        auto&     sink   = flow.emplaceBlock<qa_edit::Sink>();
        expect(flow.connect<"out", "in">(source, stage).has_value());
        expect(flow.connect<"out", "in">(stage, sink).has_value());
        const std::string stageName{stage.unique_name};

        qa_edit::JobListProbe scheduler({{"poolName", std::string(kPoolName)}});
        expect(scheduler.exchange(std::move(flow)).has_value());
        gr::MsgPortOut toScheduler;
        gr::MsgPortIn  fromScheduler;
        expect(toScheduler.connect(scheduler.msgIn).has_value());
        expect(scheduler.msgOut.connect(fromScheduler).has_value());

        expect(scheduler.changeStateTo(INITIALISED).has_value());
        expect(scheduler.changeStateTo(RUNNING).has_value());
        expect(eq(scheduler.nJobLists(), 3UZ)) << "the graph was not split into three job lists";
        const std::optional<std::size_t> heldBy = scheduler.jobListOf(stageName);
        expect(heldBy.has_value()) << "no job list holds the stage";

        qa_edit::sendMessage(toScheduler, kReplaceBlock, {{"uniqueName", stageName}, {"type", gr::meta::type_name<qa_edit::CountedTunable>()}});
        const std::optional<gr::property_map> reply = qa_edit::awaitReplyData(fromScheduler, kBlockReplaced);
        expect(reply.has_value()) << "the replacement was refused";
        const qa_edit::ReplacementRecord record = reply.has_value() ? qa_edit::replacementRecord(*reply) : qa_edit::ReplacementRecord{};
        expect(record.jobList.has_value() && record.jobList == heldBy) << "the reply does not name the job list of the replaced block";
        expect(qa_edit::awaitCondition([&scheduler, &record, &heldBy] { return scheduler.jobListOf(record.uniqueName) == heldBy; })) << "the replacement runs in another job list";
        expect(qa_edit::awaitCondition([] { return qa_edit::gCountedSamples.load() > 0UZ; })) << "the replacement never ran";

        expect(scheduler.changeStateTo(REQUESTED_STOP).has_value());
        expect(qa_edit::awaitCondition([&scheduler] { return scheduler.state() == STOPPED; })) << "the scheduler did not stop";
    };

    "a replacement inside a subgraph of a running graph runs"_test = [] {
        qa_edit::registerTestBlocks();
        qa_edit::gCountedSamples.store(0UZ);

        gr::Graph flow;
        auto      wrapper = std::make_shared<gr::GraphWrapper<gr::Graph>>();
        auto&     inner   = *wrapper->graph();
        auto&     source  = inner.emplaceBlock<qa_edit::Source>();
        auto&     stage   = inner.emplaceBlock<qa_edit::Tunable>();
        auto&     sink    = inner.emplaceBlock<qa_edit::Sink>();
        expect(inner.connect<"out", "in">(source, stage).has_value());
        expect(inner.connect<"out", "in">(stage, sink).has_value());
        const std::string stageName{stage.unique_name};
        const std::string subgraphName{flow.addBlock(wrapper)->uniqueName()};

        qa_edit::TestScheduler scheduler;
        expect(scheduler.exchange(std::move(flow)).has_value());
        gr::MsgPortOut toScheduler;
        gr::MsgPortIn  fromScheduler;
        expect(toScheduler.connect(scheduler.msgIn).has_value());
        expect(scheduler.msgOut.connect(fromScheduler).has_value());

        expect(scheduler.changeStateTo(INITIALISED).has_value());
        expect(scheduler.changeStateTo(RUNNING).has_value());

        qa_edit::sendMessage(toScheduler, kReplaceBlock, {{"uniqueName", stageName}, {"type", gr::meta::type_name<qa_edit::CountedTunable>()}, {"_targetGraph", subgraphName}});
        expect(qa_edit::awaitReplyData(fromScheduler, kBlockReplaced).has_value()) << "the replacement was refused";
        expect(qa_edit::awaitCondition([] { return qa_edit::gCountedSamples.load() > 0UZ; })) << "the replacement in the subgraph never ran";

        expect(scheduler.changeStateTo(REQUESTED_STOP).has_value());
        expect(qa_edit::awaitCondition([&scheduler] { return scheduler.state() == STOPPED; })) << "the scheduler did not stop";
    };

    // A pool of two threads splits the five blocks into two job lists, and each holds a free-running source and sink.
    // Whichever worker handles the replacement, the other one works a free-running pair unless it is held
    "a replaced block stops while no block of the run is inside work()"_test = [] {
        constexpr std::string_view kPoolName = "qa_edit_two_threads";
        gr::thread_pool::Manager::instance().replacePool(std::string(kPoolName), std::make_shared<gr::thread_pool::ThreadPoolWrapper>(std::make_unique<gr::thread_pool::BasicThreadPool>(kPoolName, gr::thread_pool::TaskType::CPU_BOUND, 2U, 2U), "CPU"));
        qa_edit::registerTestBlocks();
        qa_edit::gFreeRunningSamples.store(0UZ);
        qa_edit::gSamplesDuringStop.store(0UZ);
        qa_edit::gStopProbed.store(false);

        gr::Graph flow;
        auto&     firstSource  = flow.emplaceBlock<qa_edit::Source>();
        auto&     firstSink    = flow.emplaceBlock<qa_edit::FreeSink>();
        auto&     secondSource = flow.emplaceBlock<qa_edit::Source>();
        auto&     secondSink   = flow.emplaceBlock<qa_edit::FreeSink>();
        auto&     probe        = flow.emplaceBlock<qa_edit::StopProbe>();
        expect(flow.connect<"out", "in">(firstSource, firstSink).has_value());
        expect(flow.connect<"out", "in">(secondSource, secondSink).has_value());
        const std::string probeName{probe.unique_name};

        qa_edit::JobListProbe scheduler({{"poolName", std::string(kPoolName)}});
        expect(scheduler.exchange(std::move(flow)).has_value());
        gr::MsgPortOut toScheduler;
        gr::MsgPortIn  fromScheduler;
        expect(toScheduler.connect(scheduler.msgIn).has_value());
        expect(scheduler.msgOut.connect(fromScheduler).has_value());

        expect(scheduler.changeStateTo(INITIALISED).has_value());
        expect(scheduler.changeStateTo(RUNNING).has_value());
        expect(eq(scheduler.nJobLists(), 2UZ)) << "the graph was not split into two job lists";
        expect(qa_edit::awaitCondition([] { return qa_edit::gFreeRunningSamples.load() > 0UZ; })) << "the free-running sinks never ran";

        qa_edit::sendMessage(toScheduler, kReplaceBlock, {{"uniqueName", probeName}, {"type", gr::meta::type_name<qa_edit::Ticker>()}});
        expect(qa_edit::awaitReplyData(fromScheduler, kBlockReplaced).has_value()) << "the replacement was refused";
        expect(qa_edit::awaitCondition([] { return qa_edit::gStopProbed.load(); })) << "the replaced block's stop() hook never ran";
        expect(eq(qa_edit::gSamplesDuringStop.load(), 0UZ)) << "another block worked while the replaced block stopped";

        expect(scheduler.changeStateTo(REQUESTED_STOP).has_value());
        expect(qa_edit::awaitCondition([&scheduler] { return scheduler.state() == STOPPED; })) << "the scheduler did not stop";
    };

    // the replacement's settingsChanged() holds the edit until the stop has begun
    "a stop that begins during a replacement leaves the replacement out of the run"_test = [] {
        qa_edit::registerTestBlocks();
        qa_edit::gRacingEntered.store(false);
        {
            gr::Graph flow;
            auto&     source = flow.emplaceBlock<qa_edit::Source>();
            auto&     stage  = flow.emplaceBlock<qa_edit::Tunable>();
            auto&     sink   = flow.emplaceBlock<qa_edit::Sink>();
            expect(flow.connect<"out", "in">(source, stage).has_value());
            expect(flow.connect<"out", "in">(stage, sink).has_value());
            const std::string stageName{stage.unique_name};

            qa_edit::TestScheduler scheduler;
            expect(scheduler.exchange(std::move(flow)).has_value());
            gr::MsgPortOut toScheduler;
            gr::MsgPortIn  fromScheduler;
            expect(toScheduler.connect(scheduler.msgIn).has_value());
            expect(scheduler.msgOut.connect(fromScheduler).has_value());
            qa_edit::gSchedulerState = [&scheduler] { return scheduler.state(); };

            expect(scheduler.changeStateTo(INITIALISED).has_value());
            expect(scheduler.changeStateTo(RUNNING).has_value());

            qa_edit::sendMessage(toScheduler, kReplaceBlock, {{"uniqueName", stageName}, {"type", gr::meta::type_name<qa_edit::RacingStage>()}, {"properties", gr::property_map{{"gain", 2.0f}}}});
            expect(qa_edit::awaitCondition([] { return qa_edit::gRacingEntered.load(); })) << "the replacement's settings were never applied";
            expect(scheduler.changeStateTo(REQUESTED_STOP).has_value());

            const std::optional<gr::property_map> reply = qa_edit::awaitReplyData(fromScheduler, kBlockReplaced);
            expect(reply.has_value()) << "the replacement was refused";
            const qa_edit::ReplacementRecord record = reply.has_value() ? qa_edit::replacementRecord(*reply) : qa_edit::ReplacementRecord{};
            expect(!record.jobList.has_value()) << "the replacement joined a run whose stop had begun";
            expect(qa_edit::awaitCondition([&scheduler] { return scheduler.state() == STOPPED; })) << "the scheduler did not stop";
            for (const std::shared_ptr<gr::BlockModel>& block : scheduler.graph().blocks()) {
                if (block->uniqueName() == record.uniqueName) {
                    expect(block->state() != RUNNING) << "the stop left the replacement running";
                }
            }
        }
        qa_edit::gSchedulerState = nullptr;
    };
};
#endif

int main() { /* tests are statically registered */ }
