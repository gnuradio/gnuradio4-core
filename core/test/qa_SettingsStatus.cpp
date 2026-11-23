#include <boost/ut.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <format>
#include <limits>
#include <memory>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <gnuradio-4.0/Block.hpp>
#include <gnuradio-4.0/BlockMerging.hpp>
#include <gnuradio-4.0/BlockingSync.hpp>
#include <gnuradio-4.0/Graph.hpp>
#include <gnuradio-4.0/Port.hpp>
#include <gnuradio-4.0/Scheduler.hpp>
#include <gnuradio-4.0/Settings.hpp>

namespace qa_settings_status {

using namespace boost::ut;
using gr::Annotated;
using gr::property_map;
using gr::settings::Rejected;
using gr::settings::Values;

// a level meter that reports its peak, an `Annotated`, and its sample count, a plain member, in its struct `status`
struct StatusMeter : gr::Block<StatusMeter> {
    gr::PortIn<float> in;

    struct Settings {
        Annotated<float, gr::Unit<"dB">> limit = 0.f;

        GR_MAKE_REFLECTABLE(Settings, limit);
    } settings;

    struct Status {
        Annotated<float, gr::Unit<"dB">, gr::Doc<"largest level seen">> peak  = -100.f;
        std::uint64_t                                                   count = 0U;

        GR_MAKE_REFLECTABLE(Status, peak, count);
    } status;

    GR_MAKE_REFLECTABLE(StatusMeter, in, settings, status);

    explicit StatusMeter(property_map init = {}) : gr::Block<StatusMeter>(std::move(init)) {}

    void stop() { status.count = 0U; }

    gr::work::Status processBulk(gr::InputSpanLike auto& inSpan) {
        const float largest = std::ranges::max(inSpan);
        if (largest > status.peak.value) {
            status.peak = largest;
        }
        status.count += inSpan.size();
        std::ignore = inSpan.consume(inSpan.size());
        return gr::work::Status::OK;
    }

    void applySettings(const Settings& s) { status.peak = s.limit.value; }
};

struct QuietStatusSink : gr::Block<QuietStatusSink> {
    gr::PortIn<float> in;

    struct Status {
        float first  = 0.f;
        float second = 0.f;
        float third  = 0.f;
        float fourth = 0.f;

        GR_MAKE_REFLECTABLE(Status, first, second, third, fourth);
    } status;

    GR_MAKE_REFLECTABLE(QuietStatusSink, in, status);

    explicit QuietStatusSink(property_map init = {}) : gr::Block<QuietStatusSink>(std::move(init)) {}

    [[nodiscard]] constexpr gr::work::Status processBulk(gr::InputSpanLike auto& inSpan) noexcept {
        std::ignore = inSpan.consume(inSpan.size());
        return gr::work::Status::OK;
    }
};

// readings a device did not state, held as not-a-number: a number, one element of a list and one part of a complex number
struct NaNReadings : gr::Block<NaNReadings> {
    gr::PortIn<float> in;

    struct Status {
        float               width  = std::numeric_limits<float>::quiet_NaN();
        std::vector<float>  gains  = {std::numeric_limits<float>::quiet_NaN(), 2.f};
        std::complex<float> offset = {std::numeric_limits<float>::quiet_NaN(), 0.f};

        GR_MAKE_REFLECTABLE(Status, width, gains, offset);
    } status;

    GR_MAKE_REFLECTABLE(NaNReadings, in, status);

    explicit NaNReadings(property_map init = {}) : gr::Block<NaNReadings>(std::move(init)) {}

    [[nodiscard]] constexpr gr::work::Status processBulk(gr::InputSpanLike auto& inSpan) noexcept {
        std::ignore = inSpan.consume(inSpan.size());
        return gr::work::Status::OK;
    }
};

// counts its work calls with `++`, sums its samples with `+=` and keeps its last samples by a member call
struct CountingSink : gr::Block<CountingSink> {
    gr::PortIn<float> in;

    struct Status {
        std::uint64_t                 calls = 0U;
        Annotated<double>             total = 0.;
        Annotated<std::vector<float>> last{};

        GR_MAKE_REFLECTABLE(Status, calls, total, last);
    } status;

    GR_MAKE_REFLECTABLE(CountingSink, in, status);

    explicit CountingSink(property_map init = {}) : gr::Block<CountingSink>(std::move(init)) {}

    gr::work::Status processBulk(gr::InputSpanLike auto& inSpan) {
        ++status.calls;
        for (const float sample : inSpan) {
            status.total += static_cast<double>(sample);
        }
        status.last->assign(inSpan.begin(), inSpan.end());
        std::ignore = inSpan.consume(inSpan.size());
        return gr::work::Status::OK;
    }
};

// a blocking pass that counts its samples in its work function and its stops in its stop hook, which the scheduler runs on
// its own thread
struct StopCountingPass : gr::Block<StopCountingPass> {
    gr::PortIn<float>  in;
    gr::PortOut<float> out;

    struct Status {
        std::uint64_t count = 0U;
        std::uint64_t stops = 0U;

        GR_MAKE_REFLECTABLE(Status, count, stops);
    } status;

    GR_MAKE_REFLECTABLE(StopCountingPass, in, out, status);

    explicit StopCountingPass(property_map init = {}) : gr::Block<StopCountingPass>(std::move(init)) {}

    [[nodiscard]] constexpr bool isBlocking() const noexcept { return true; }

    void stop() { ++status.stops; }

    [[nodiscard]] float processOne(float value) {
        ++status.count;
        return value;
    }
};

// passes its input on and counts the samples in its `Status`; no lifecycle hook
struct CountingPass : gr::Block<CountingPass> {
    gr::PortIn<float>  in;
    gr::PortOut<float> out;

    struct Status {
        std::uint64_t count = 0U;

        GR_MAKE_REFLECTABLE(Status, count);
    } status;

    GR_MAKE_REFLECTABLE(CountingPass, in, out, status);

    explicit CountingPass(property_map init = {}) : gr::Block<CountingPass>(std::move(init)) {}

    [[nodiscard]] float processOne(float value) {
        ++status.count;
        return value;
    }
};

struct OnesSource : gr::Block<OnesSource> {
    gr::PortOut<float> out;

    GR_MAKE_REFLECTABLE(OnesSource, out);

    [[nodiscard]] constexpr float processOne() const noexcept { return 1.f; }
};

// gives `nSamples` ones, then ends its stream
struct FiniteOnes : gr::Block<FiniteOnes> {
    gr::PortOut<float> out;

    GR_MAKE_REFLECTABLE(FiniteOnes, out);

    std::size_t nLeft = 1000UZ;

    gr::work::Status processBulk(gr::OutputSpanLike auto& outSpan) {
        const std::size_t nSamples = std::min(nLeft, outSpan.size());
        std::ranges::fill_n(outSpan.begin(), static_cast<std::ptrdiff_t>(nSamples), 1.f);
        outSpan.publish(nSamples);
        nLeft -= nSamples;
        return nLeft == 0UZ ? gr::work::Status::DONE : gr::work::Status::OK;
    }
};

inline std::atomic<std::size_t> gTallied{0UZ}; // the samples TallySink took, read from the test's thread

struct TallySink : gr::Block<TallySink> {
    gr::PortIn<float> in;

    GR_MAKE_REFLECTABLE(TallySink, in);

    void processOne(float /*value*/) { gTallied.fetch_add(1UZ, std::memory_order_relaxed); }
};

// the core's settings of a scheduler and one of HeldKeyScheduler's own, which a running scheduler holds for its next start
struct HeldKeySettings : gr::scheduler::SchedulerSettings {
    Annotated<gr::Size_t, gr::Doc<"samples a worker keeps">> spare = 0U;

    GR_MAKE_REFLECTABLE(HeldKeySettings, spare);
};

struct HeldKeyScheduler : gr::scheduler::SchedulerBase<HeldKeyScheduler, gr::scheduler::ExecutionPolicy::singleThreaded, gr::profiling::null::Profiler, HeldKeySettings> {
    using Base = gr::scheduler::SchedulerBase<HeldKeyScheduler, gr::scheduler::ExecutionPolicy::singleThreaded, gr::profiling::null::Profiler, HeldKeySettings>;
    using Base::Base;

    void customInit() {
        const gr::Graph flatGraph = gr::graph::flatten(*this->_graph);
        const auto      blocks    = flatGraph.blocks();

        std::lock_guard lock(this->_executionOrderMutex);
        std::lock_guard guard(this->_adoptionBlocksMutex);
        this->_adoptionBlocks.clear();
        this->_adoptionBlocks.resize(1UZ);
        this->_executionOrder->clear();
        this->_executionOrder->emplace_back(blocks.begin(), blocks.end());
    }
};

// a single-threaded scheduler whose start hook counts its starts and stops the run; under this policy the hook runs
// after the worker has returned
struct StoppingStartHook : gr::scheduler::SchedulerBase<StoppingStartHook, gr::scheduler::ExecutionPolicy::singleThreaded> {
    using SchedulerBase::SchedulerBase;

    std::size_t nStarts       = 0UZ; // read once the run has returned
    std::size_t nStopsRefused = 0UZ; // the hooks that found the run stopped or whose stop was refused, read once the run has returned

    void customInit() {
        const gr::Graph flatGraph = gr::graph::flatten(*this->_graph);
        const auto      blocks    = flatGraph.blocks();

        std::lock_guard lock(this->_executionOrderMutex);
        std::lock_guard guard(this->_adoptionBlocksMutex);
        this->_adoptionBlocks.clear();
        this->_adoptionBlocks.resize(1UZ);
        this->_executionOrder->clear();
        this->_executionOrder->emplace_back(blocks.begin(), blocks.end());
    }

    void customStart() {
        ++nStarts;
        if (this->state() != gr::lifecycle::State::RUNNING || !this->changeStateTo(gr::lifecycle::State::REQUESTED_STOP).has_value()) {
            ++nStopsRefused;
        }
    }
};

// a scheduler of ones into a TallySink, set up and running on `runner` until the sink took 64 samples
template<typename TScheduler>
void runOnes(TScheduler& sched, std::thread& runner) {
    gTallied.store(0UZ);
    expect(fatal(sched.check({}).has_value()));
    {
        gr::Graph flow;
        auto&     ones = flow.emplaceBlock<OnesSource>();
        auto&     sink = flow.emplaceBlock<TallySink>();
        expect(fatal(flow.connect<"out", "in">(ones, sink).has_value()));
        expect(fatal(sched.exchange(std::move(flow)).has_value()));
    }
    runner              = std::thread([&sched] { std::ignore = sched.runAndWait(); });
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (gTallied.load(std::memory_order_relaxed) < 64UZ && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::yield();
    }
    expect(ge(gTallied.load(std::memory_order_relaxed), 64UZ)) << "the graph does not run";
}

// defines its own `work()`, as a block whose samples another thread produces does, and counts its calls in its `Status`
struct OwnWorkCounter : gr::Block<OwnWorkCounter> {
    struct Status {
        std::uint64_t calls = 0U;

        GR_MAKE_REFLECTABLE(Status, calls);
    } status;

    GR_MAKE_REFLECTABLE(OwnWorkCounter, status);

    explicit OwnWorkCounter(property_map init = {}) : gr::Block<OwnWorkCounter>(std::move(init)) {}

    gr::work::Result work(std::size_t requestedWork = std::numeric_limits<std::size_t>::max()) noexcept {
        ++status.calls;
        return {requestedWork, 0UZ, gr::work::Status::OK};
    }
};

// sends one message at its output in each work call, three in all, then ends
struct MessageSender : gr::Block<MessageSender> {
    gr::MsgPortOut out;

    GR_MAKE_REFLECTABLE(MessageSender, out);

    std::size_t nLeft = 3UZ;

    gr::work::Status processBulk() {
        if (nLeft > 0UZ) {
            gr::sendMessage<gr::message::Command::Notify>(out, "", "event", property_map{});
            --nLeft;
        }
        return nLeft == 0UZ ? gr::work::Status::DONE : gr::work::Status::OK;
    }
};

// counts the messages at its own message port in its `Status`, in the handler the scheduler calls apart from the work
// call; its work call ends at once
struct MessageCounter : gr::Block<MessageCounter> {
    gr::MsgPortIn messages;

    struct Status {
        std::uint64_t n_messages = 0U;

        GR_MAKE_REFLECTABLE(Status, n_messages);
    } status;

    GR_MAKE_REFLECTABLE(MessageCounter, messages, status);

    explicit MessageCounter(property_map init = {}) : gr::Block<MessageCounter>(std::move(init)) {}

    void processMessages(const gr::MsgPortInBuiltin& port, std::span<const gr::Message> requests) { gr::BlockBase::processMessages(port, requests); }

    void processMessages(gr::MsgPortIn& /*port*/, std::span<const gr::Message> received) { status.n_messages += received.size(); }

    [[nodiscard]] constexpr gr::work::Status processBulk() const noexcept { return gr::work::Status::DONE; }
};

// a source paced by BlockingSync, whose three timing values are members of its `Settings`; with no timer thread its work
// call reads the wall clock
struct SettingsPacedSource : gr::Block<SettingsPacedSource>, gr::BlockingSync<SettingsPacedSource> {
    gr::PortOut<float> out;

    struct Settings {
        Annotated<float>      sample_rate         = 1'000'000.f;
        Annotated<gr::Size_t> chunk_size          = 7U;
        Annotated<bool>       use_internal_thread = false;

        GR_MAKE_REFLECTABLE(Settings, sample_rate, chunk_size, use_internal_thread);
    } settings;

    GR_MAKE_REFLECTABLE(SettingsPacedSource, out, settings);

    explicit SettingsPacedSource(property_map init = {}) : gr::Block<SettingsPacedSource>(std::move(init)) {}

    void start() { this->blockingSyncStart(); }
    void stop() { this->blockingSyncStop(); }

    gr::work::Status processBulk(gr::OutputSpanLike auto& outSpan) {
        const std::size_t nSamples = this->syncSamples(outSpan);
        std::ranges::fill_n(outSpan.begin(), static_cast<std::ptrdiff_t>(nSamples), 1.f);
        outSpan.publish(nSamples);
        return gr::work::Status::OK;
    }
};

// its twin with only the sample rate in its `Settings`: the chunk is a tenth of a second, and a timer thread paces it
struct RateOnlySource : gr::Block<RateOnlySource>, gr::BlockingSync<RateOnlySource> {
    gr::PortOut<float> out;

    struct Settings {
        Annotated<float> sample_rate = 1000.f;

        GR_MAKE_REFLECTABLE(Settings, sample_rate);
    } settings;

    GR_MAKE_REFLECTABLE(RateOnlySource, out, settings);

    explicit RateOnlySource(property_map init = {}) : gr::Block<RateOnlySource>(std::move(init)) {}

    gr::work::Status processBulk(gr::OutputSpanLike auto& outSpan) {
        outSpan.publish(0UZ);
        return gr::work::Status::OK;
    }
};

// a scheduler as a block's settings see it, at the notification level `info`: a watcher of the block's notifications.
// It keeps each notification it receives
struct WatchingLink final : gr::settings::detail::SchedulerLink {
    std::vector<gr::settings::Notification> received{};

    void                                                         wake() override {}
    void                                                         push(gr::settings::Notification notification) override { received.push_back(std::move(notification)); }
    [[nodiscard]] bool                                           isWorkerThread() const override { return false; }
    [[nodiscard]] std::optional<gr::settings::NotificationLevel> level() const override { return gr::settings::NotificationLevel::info; }
    [[nodiscard]] std::chrono::milliseconds                      defaultTimeout() const override { return std::chrono::milliseconds(1000); }

    [[nodiscard]] std::size_t changesOf(std::string_view key) const {
        return static_cast<std::size_t>(std::ranges::count_if(received, [key](const gr::settings::Notification& notification) { return notification.what == gr::settings::NotificationKind::changed && notification.key == key; }));
    }
};

// records in its apply hook the values `settings` held before the change and the values it holds; the hook throws for a
// gain above `throwAbove`
struct OldStage : gr::Block<OldStage> {
    struct Settings {
        Annotated<float, gr::Unit<"dB">> gain = 0.f;
        Annotated<std::string>           mode = std::string("linear");

        GR_MAKE_REFLECTABLE(Settings, gain, mode);
    } settings;

    GR_MAKE_REFLECTABLE(OldStage, settings);

    struct Seen {
        float       oldGain = -1.f;
        std::string oldMode{};
        float       gain = -1.f;
        std::string mode{};

        bool operator==(const Seen&) const = default;
    };

    Seen  seen{};
    int   applies    = 0;
    float throwAbove = 100.f;

    explicit OldStage(property_map init = {}) : gr::Block<OldStage>(std::move(init)) {}

    void applySettings(const Settings& s, const Settings& old) {
        ++applies;
        seen = Seen{.oldGain = old.gain.value, .oldMode = old.mode.value, .gain = s.gain.value, .mode = s.mode.value};
        if (s.gain.value > throwAbove) {
            throw std::runtime_error("the gain exceeds the device's range");
        }
    }
};

// takes `trigger` as a command: its apply hook counts each trigger in a member of its own and assigns the setting false,
// so a host reads false always
struct TriggeredStage : gr::Block<TriggeredStage> {
    struct Settings {
        Annotated<bool> trigger = false;

        GR_MAKE_REFLECTABLE(Settings, trigger);
    } settings;

    GR_MAKE_REFLECTABLE(TriggeredStage, settings);

    int  nTriggers  = 0;
    bool oldTrigger = true; // the value before the last apply

    explicit TriggeredStage(property_map init = {}) : gr::Block<TriggeredStage>(std::move(init)) {}

    // clears the trigger once it fires, so each `true` a caller sends fires once
    void applySettings(Settings& s, const Settings& old) {
        oldTrigger = old.trigger.value;
        if (s.trigger.value) {
            ++nTriggers;
            s.trigger = false;
        }
    }
};

// the value a saved graph writes for `key`, or nothing for a key it leaves out
template<typename T, typename TBlock>
[[nodiscard]] std::optional<T> savedValueOf(const TBlock& block, std::string_view key) {
    const property_map saved = gr::settings::detail::FrameworkAccess::savedValues(gr::settings::detail::settingsOf(block));
    const auto         it    = saved.find(std::pmr::string(key));
    if (it == saved.end()) {
        return std::nullopt;
    }
    const T* value = it->second.template get_if<T>();
    return value != nullptr ? std::optional<T>(*value) : std::nullopt;
}

// connects `upstream` to the block's input and starts the block
template<typename TSink>
void startBehind(TSink& sink, gr::PortOut<float>& upstream) {
    expect(fatal(upstream.connect(sink.in).has_value()));
    sink.init(std::make_shared<gr::Sequence>());
    expect(fatal(sink.changeStateTo(gr::lifecycle::State::RUNNING).has_value()));
}

void publishSamples(gr::PortOut<float>& port, std::size_t nSamples, float value) {
    auto span = port.reserve<gr::SpanReleasePolicy::ProcessAll>(nSamples);
    std::ranges::fill(span, value);
}

template<typename TMeter>
[[nodiscard]] std::string readOf(const TMeter& meter) {
    const std::expected<float, Rejected>         peak  = meter.template get<float>("peak");
    const std::expected<std::uint64_t, Rejected> count = meter.template get<std::uint64_t>("count");
    if (!peak || !count) {
        return gr::settings::describeRejected(!peak ? peak.error() : count.error());
    }
    return std::format("peak {} count {}", *peak, *count);
}

template<typename TResult>
[[nodiscard]] std::string refusalOf(const TResult& result) {
    return result ? std::string("accepted") : gr::settings::describeRejected(result.error());
}

const suite<"SettingsStatus: the struct Status"> statusTests = [] {
    "a set of a status value is refused as the block's status"_test = [] {
        StatusMeter meter;
        expect(meter.check({}).has_value());
        expect(eq(refusalOf(meter.set("peak", 3.f)), std::string("peak: read-only status")));
        expect(eq(readOf(meter), std::string("peak 0 count 0"))) << "the refused set changed the value a read returns";
        expect(eq(refusalOf(meter.setTagCouplings(property_map{{"peak", std::string("qa:peak")}})), std::string("peak: read-only status"))) << "a tag coupling";
    };

    "a status value is no writable member and its metadata says read-only"_test = [] {
        StatusMeter meter;
        expect(meter.check({}).has_value());
        const std::set<std::string> writable = meter.writableMembers();
        expect(writable.contains("limit") && !writable.contains("peak") && !writable.contains("count"));
        const property_map& metadata = gr::settings::detail::metaInformationOf(meter);
        for (std::string_view field : {"peak::access", "peak::unit", "peak::documentation", "count::type"}) {
            expect(metadata.contains(field)) << field;
        }
        expect(metadata.contains("peak::access") && metadata.at("peak::access") == gr::pmt::Value(std::string("read_only")));
    };
};

const suite<"SettingsStatus: core copies the Status"> copyTests = [] {
    "a host reads the block's Status after an apply, a work call and a stop"_test = [] {
        gr::PortOut<float> upstream;
        StatusMeter        meter;
        expect(meter.check({}).has_value());
        expect(eq(readOf(meter), std::string("peak 0 count 0"))) << "after the setup";

        expect(meter.set("limit", -50.f).has_value());
        expect(eq(readOf(meter), std::string("peak -50 count 0"))) << "after the apply hook";

        startBehind(meter, upstream);
        expect(eq(readOf(meter), std::string("peak -50 count 0"))) << "a host reads before the work call";
        publishSamples(upstream, 4UZ, 3.f);
        std::ignore = meter.work();
        expect(eq(readOf(meter), std::string("peak 3 count 4"))) << "after the work call";

        expect(meter.changeStateTo(gr::lifecycle::State::REQUESTED_STOP).has_value());
        expect(eq(readOf(meter), std::string("peak 3 count 0"))) << "after the stop hook";
    };

    "a status value that stays not-a-number keeps the value the block published on its key"_test = [] {
        using gr::settings::detail::FrameworkAccess;
        using gr::settings::detail::settingsOf;
        constexpr float kNaN = std::numeric_limits<float>::quiet_NaN();
        NaNReadings     readings;
        expect(fatal(readings.check({}).has_value()));
        expect(std::isnan(readings.get<float>("width").value_or(0.f))) << "the default";
        FrameworkAccess::publish(settingsOf(readings), "width", gr::pmt::Value(2.5f));
        FrameworkAccess::publish(settingsOf(readings), "gains", gr::pmt::Value(std::vector<float>{1.f, 2.f}));
        FrameworkAccess::publish(settingsOf(readings), "offset", gr::pmt::Value(std::complex<float>{1.f, 1.f}));

        FrameworkAccess::copyStatus(settingsOf(readings));
        expect(eq(readings.get<float>("width").value_or(0.f), 2.5f)) << "a not-a-number that stays one keeps the published width";
        expect(readings.get<std::vector<float>>("gains").value_or(std::vector<float>{}) == std::vector<float>{1.f, 2.f}) << "a list that holds a not-a-number";
        expect(eq(readings.get<std::complex<float>>("offset").value_or(std::complex<float>{}).real(), 1.f)) << "a complex number with a not-a-number part";

        readings.status.gains = {kNaN, 3.f};
        FrameworkAccess::copyStatus(settingsOf(readings));
        expect(eq(readings.get<float>("width").value_or(0.f), 2.5f)) << "another key changed";
        const std::vector<float> gains = readings.get<std::vector<float>>("gains").value_or(std::vector<float>{});
        expect(eq(gains.size(), 2UZ) && std::isnan(gains[0]) && eq(gains[1], 3.f)) << "a changed element replaces the published list";

        readings.status.width = 4.f;
        FrameworkAccess::copyStatus(settingsOf(readings));
        expect(eq(readings.get<float>("width").value_or(0.f), 4.f)) << "a number in place of the not-a-number";

        readings.status.width = kNaN;
        FrameworkAccess::copyStatus(settingsOf(readings));
        expect(std::isnan(readings.get<float>("width").value_or(0.f))) << "a not-a-number in place of a number";
    };

    "with no watcher a work call copies the Status only after a host read a status value"_test = [] {
        gr::PortOut<float> upstream;
        StatusMeter        meter;
        startBehind(meter, upstream);
        publishSamples(upstream, 4UZ, 3.f);
        std::ignore = meter.work();
        expect(eq(readOf(meter), std::string("peak 0 count 0"))) << "no host read since the start's copy";
        publishSamples(upstream, 2UZ, 1.f);
        std::ignore = meter.work();
        expect(eq(readOf(meter), std::string("peak 3 count 6"))) << "the read before the work call has it copy";
        publishSamples(upstream, 2UZ, 1.f);
        std::ignore = meter.work();
        publishSamples(upstream, 2UZ, 1.f);
        std::ignore = meter.work();
        expect(eq(readOf(meter), std::string("peak 3 count 8"))) << "one read has the next work call copy, and no later one";
    };

    "++, += and a member call on status values reach a host"_test = [] {
        gr::PortOut<float> upstream;
        CountingSink       sink;
        startBehind(sink, upstream);
        expect(eq(sink.get<std::uint64_t>("calls").value_or(9U), 0U));
        publishSamples(upstream, 3UZ, 2.f);
        std::ignore                                = sink.work();
        const std::expected<Values, Rejected> read = sink.get();
        expect(fatal(read.has_value()));
        expect(eq(read->at("calls").as<std::uint64_t>().value_or(0U), 1U)) << "++";
        expect(eq(read->at("total").as<double>().value_or(0.), 6.)) << "+=";
        expect(read->at("last").as<std::vector<float>>().value_or(std::vector<float>{}) == std::vector<float>{2.f, 2.f, 2.f}) << "a member call";
    };

    "with a watcher a work call announces a value only when it differs from the copy before, a value set back included"_test = [] {
        WatchingLink       watcher;
        gr::PortOut<float> upstream;
        QuietStatusSink    sink;
        gr::settings::detail::FrameworkAccess::attachScheduler(gr::settings::detail::settingsOf(sink), &watcher);
        startBehind(sink, upstream);
        const auto workOnce = [&upstream, &sink] {
            publishSamples(upstream, 2UZ, 1.f);
            std::ignore = sink.work();
        };
        workOnce();
        workOnce();
        expect(eq(watcher.changesOf("first"), 0UZ)) << "an unchanged value";
        sink.status.first = 2.f;
        workOnce();
        workOnce();
        expect(eq(watcher.changesOf("first"), 1UZ)) << "one change, then none";
        sink.status.first = 0.f;
        workOnce();
        expect(eq(watcher.changesOf("first"), 2UZ)) << "the value set back to the one before";
        expect(eq(sink.get<float>("first").value_or(-1.f), 0.f));
        sink.status.second = std::numeric_limits<float>::quiet_NaN();
        workOnce();
        workOnce();
        expect(eq(watcher.changesOf("second"), 1UZ)) << "a NaN is the same value at the next work call";
        expect(eq(watcher.changesOf("third"), 0UZ) && eq(watcher.changesOf("fourth"), 0UZ));
        gr::settings::detail::FrameworkAccess::detachScheduler(gr::settings::detail::settingsOf(sink), &watcher);
    };
};

const suite<"SettingsStatus: the apply hook takes the values before"> oldTests = [] {
    using Seen = OldStage::Seen;

    "at the setup old holds the declared defaults and settings the constructed values"_test = [] {
        OldStage stage(property_map{{"gain", 2.f}});
        expect(stage.check({}).has_value());
        expect(stage.seen == Seen{.oldGain = 0.f, .oldMode = "linear", .gain = 2.f, .mode = "linear"});
    };

    "after a set of one setting old holds the values before it, the other setting's included"_test = [] {
        OldStage stage;
        expect(stage.check({}).has_value());
        expect(stage.set("gain", 6.f).has_value());
        expect(stage.seen == Seen{.oldGain = 0.f, .oldMode = "linear", .gain = 6.f, .mode = "linear"}) << "a set of the gain";
        expect(stage.set("mode", std::string("clip")).has_value());
        expect(stage.seen == Seen{.oldGain = 6.f, .oldMode = "linear", .gain = 6.f, .mode = "clip"}) << "a set of the mode";
        expect(stage.setTagCouplings(property_map{{"gain", std::string("qa:gain")}}).has_value());
        expect(gr::settings::detail::FrameworkAccess::tagEnters(gr::settings::detail::settingsOf(stage), property_map{{"qa:gain", 3.f}}).empty());
        std::ignore = gr::settings::detail::FrameworkAccess::applyStagedParameters(gr::settings::detail::settingsOf(stage));
        expect(stage.seen == Seen{.oldGain = 6.f, .oldMode = "clip", .gain = 3.f, .mode = "clip"}) << "a change by tag";
    };

    "a resend of the value in force runs no apply hook"_test = [] {
        OldStage stage;
        expect(stage.check({}).has_value());
        expect(stage.set("gain", 6.f).has_value());
        const int  applies = stage.applies;
        const Seen seen    = stage.seen;
        expect(stage.set("gain", 6.f).has_value());
        expect(eq(stage.applies, applies));
        expect(stage.seen == seen);
    };

    "a hook that throws leaves settings as old"_test = [] {
        OldStage stage;
        expect(stage.check({}).has_value());
        expect(stage.set(property_map{{"gain", 6.f}, {"mode", std::string("clip")}}).has_value());
        std::ignore = stage.set(property_map{{"gain", 200.f}, {"mode", std::string("fold")}});
        expect(stage.seen == Seen{.oldGain = 6.f, .oldMode = "clip", .gain = 200.f, .mode = "fold"}) << "the hook saw the change";
        expect(eq(stage.settings.gain.value, stage.seen.oldGain) && eq(stage.settings.mode.value, stage.seen.oldMode)) << "settings after the throw";
        expect(eq(stage.get<float>("gain").value_or(-1.f), 6.f));
        expect(stage.set("gain", 7.f).has_value());
        expect(stage.seen == Seen{.oldGain = 6.f, .oldMode = "clip", .gain = 7.f, .mode = "clip"}) << "the next apply after the throw";
    };
};

const suite<"SettingsStatus: a setting the apply hook assigns"> assignedTests = [] {
    using gr::settings::detail::FrameworkAccess;
    using gr::settings::detail::settingsOf;

    "a constructed trigger the setup's hook assigns false reads false, and a saved graph writes false"_test = [] {
        TriggeredStage stage(property_map{{"trigger", true}});
        expect(fatal(stage.check({}).has_value()));
        expect(eq(stage.nTriggers, 1));
        expect(eq(stage.get<bool>("trigger").value_or(true), false));
        expect(eq(savedValueOf<bool>(stage, "trigger").value_or(true), false)) << "a saved graph writes the value the hook assigned";
    };

    "a trigger the hook assigns false reads false by get, after a settings message and in a saved graph, with no notification"_test = [] {
        WatchingLink   watcher;
        TriggeredStage stage;
        FrameworkAccess::attachScheduler(settingsOf(stage), &watcher);
        expect(fatal(stage.check({}).has_value()));
        expect(fatal(stage.set("trigger", true).has_value()));
        expect(eq(stage.nTriggers, 1));
        expect(eq(stage.get<bool>("trigger").value_or(true), false)) << "by get";
        expect(eq(savedValueOf<bool>(stage, "trigger").value_or(true), false)) << "in a saved graph";
        expect(eq(watcher.changesOf("trigger"), 0UZ)) << "the assignment is announced";

        expect(fatal(stage.set("trigger", true).has_value()));
        expect(eq(stage.nTriggers, 2)) << "the first value sent again is a change again";
        expect(eq(stage.oldTrigger, false)) << "old holds the value the hook assigned";

        gr::MsgPortOut toStage;
        expect(fatal(toStage.connect(stage.msgIn).has_value()));
        gr::sendMessage<gr::message::Command::Set>(toStage, "", gr::block::property::kSetting, property_map{{"trigger", true}});
        stage.processScheduledMessages();
        expect(eq(stage.nTriggers, 3)) << "a settings message triggers";
        expect(eq(stage.get<bool>("trigger").value_or(true), false)) << "after a settings message";
        expect(eq(savedValueOf<bool>(stage, "trigger").value_or(true), false)) << "in a saved graph after a settings message";
        expect(eq(watcher.changesOf("trigger"), 0UZ)) << "no assignment is announced";
        FrameworkAccess::detachScheduler(settingsOf(stage), &watcher);
    };
};

const suite<"SettingsStatus: a merged block and a blocking block"> gapTests = [] {
    "a host reads once after a run the value a blocking block's stop hook assigned"_test = [] {
        gTallied.store(0UZ);
        gr::scheduler::Simple<> sched;
        StopCountingPass*       counter = nullptr;
        {
            gr::Graph flow;
            auto&     ones = flow.emplaceBlock<OnesSource>();
            counter        = &flow.emplaceBlock<StopCountingPass>();
            auto& sink     = flow.emplaceBlock<TallySink>();
            expect(fatal(flow.connect<"out", "in">(ones, *counter).has_value()));
            expect(fatal(flow.connect<"out", "in">(*counter, sink).has_value()));
            expect(fatal(sched.exchange(std::move(flow)).has_value()));
        }
        std::thread runner([&sched] { std::ignore = sched.runAndWait(); });
        const auto  deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (gTallied.load(std::memory_order_relaxed) < 64UZ && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::yield();
        }
        std::ignore = sched.changeStateTo(gr::lifecycle::State::REQUESTED_STOP);
        runner.join();
        expect(ge(gTallied.load(std::memory_order_relaxed), 64UZ)) << "the graph does not run";
        expect(eq(counter->get<std::uint64_t>("stops").value_or(9U), 1U)) << "the one read after the run";
    };

    "a host reads after a scheduled run the last values of a block that has no lifecycle hook"_test = [] {
        gTallied.store(0UZ);
        gr::scheduler::Simple<> sched;
        CountingPass*           counter = nullptr;
        {
            gr::Graph flow;
            auto&     source = flow.emplaceBlock<FiniteOnes>();
            counter          = &flow.emplaceBlock<CountingPass>();
            auto& sink       = flow.emplaceBlock<TallySink>();
            expect(fatal(flow.connect<"out", "in">(source, *counter).has_value()));
            expect(fatal(flow.connect<"out", "in">(*counter, sink).has_value()));
            expect(fatal(sched.exchange(std::move(flow)).has_value()));
        }
        expect(fatal(sched.runAndWait().has_value()));
        expect(eq(gTallied.load(std::memory_order_relaxed), 1000UZ)) << "the graph does not run to its end";
        expect(eq(counter->get<std::uint64_t>("count").value_or(0U), 1000U)) << "no host read during the run: the end of the run copies";
    };

    "a host reads once after a run the count a handler made after the block's work function returned DONE"_test = [] {
        gr::scheduler::Simple<> sched;
        MessageCounter*         counter = nullptr;
        {
            gr::Graph flow;
            auto&     sender = flow.emplaceBlock<MessageSender>();
            counter          = &flow.emplaceBlock<MessageCounter>();
            expect(fatal(flow.connect<"out", "messages">(sender, *counter).has_value()));
            expect(fatal(sched.exchange(std::move(flow)).has_value()));
        }
        expect(fatal(sched.runAndWait().has_value()));
        expect(eq(counter->status.n_messages, std::uint64_t{3U})) << "the handler does not count each message";
        expect(eq(counter->get<std::uint64_t>("n_messages").value_or(9U), 3U)) << "the one read after the run";
    };

    "a block with its own work() reaches a host that reads twice during a run and once after it"_test = [] {
        gTallied.store(0UZ);
        gr::scheduler::Simple<> sched;
        OwnWorkCounter*         counter = nullptr;
        {
            gr::Graph flow;
            counter    = &flow.emplaceBlock<OwnWorkCounter>();
            auto& ones = flow.emplaceBlock<OnesSource>();
            auto& sink = flow.emplaceBlock<TallySink>();
            expect(fatal(flow.connect<"out", "in">(ones, sink).has_value()));
            expect(fatal(sched.exchange(std::move(flow)).has_value()));
        }
        std::thread runner([&sched] { std::ignore = sched.runAndWait(); });
        const auto  deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        const auto  runUntil = [&deadline](std::size_t nTallied) {
            while (gTallied.load(std::memory_order_relaxed) < nTallied && std::chrono::steady_clock::now() < deadline) {
                std::this_thread::yield();
            }
        };
        runUntil(64UZ);
        const std::uint64_t first = counter->get<std::uint64_t>("calls").value_or(0U);
        runUntil(gTallied.load(std::memory_order_relaxed) + 4UZ * 65'536UZ); // a pass of the sink takes at most one buffer of samples
        const std::uint64_t second = counter->get<std::uint64_t>("calls").value_or(0U);
        std::ignore                = sched.changeStateTo(gr::lifecycle::State::REQUESTED_STOP);
        runner.join();
        expect(gt(second, first)) << "a work call after the first read copies";
        expect(eq(counter->get<std::uint64_t>("calls").value_or(0U), counter->status.calls)) << "the one read after the run";
    };

    "a running scheduler's held key set without a wait answers pending for the next start, reads its value in force and takes the held value at the next start"_test = [] {
        HeldKeyScheduler sched;
        std::thread      runner;
        runOnes(sched, runner);
        const std::expected<gr::settings::Values, gr::settings::Rejected> held    = sched.set("spare", gr::Size_t{5U}, gr::settings::noWait);
        const std::expected<gr::Size_t, gr::settings::Rejected>           inForce = sched.get<gr::Size_t>("spare");
        const std::expected<gr::settings::Values, gr::settings::Rejected> waited  = sched.set("settings_timeout_ms", gr::Size_t{250U});
        std::ignore                                                               = sched.changeStateTo(gr::lifecycle::State::REQUESTED_STOP);
        runner.join();
        const gr::settings::Value nextStart{.state = gr::settings::Value::State::pending, .value = {}, .reason = std::string(gr::settings::kPendingNextStart)};
        expect(held == gr::settings::Values{{"spare", nextStart}}) << "the answer names the next start";
        expect(inForce == gr::Size_t{0U}) << "a plain read returns the held value";
        expect(waited == gr::settings::Values{{"settings_timeout_ms", nextStart}}) << "a call that waits answers its settings timeout pending for the next start";

        expect(fatal(sched.changeStateTo(gr::lifecycle::State::INITIALISED).has_value()));
        gTallied.store(0UZ);
        runner              = std::thread([&sched] { std::ignore = sched.runAndWait(); });
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (gTallied.load(std::memory_order_relaxed) < 64UZ && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::yield();
        }
        const std::expected<gr::Size_t, gr::settings::Rejected> started = sched.get<gr::Size_t>("spare");
        std::ignore                                                     = sched.changeStateTo(gr::lifecycle::State::REQUESTED_STOP);
        runner.join();
        expect(started == gr::Size_t{5U}) << "the next start dropped the held key";
        expect(eq(sched.settings.settings_timeout_ms.value, 250U)) << "the next start dropped the held timeout";
    };

    "a single-threaded scheduler's start hook stops the run that ended, and the start returns"_test = [] {
        StoppingStartHook sched;
        expect(fatal(sched.check({}).has_value()));
        {
            gr::Graph flow;
            auto&     ones = flow.emplaceBlock<FiniteOnes>();
            auto&     sink = flow.emplaceBlock<TallySink>();
            expect(fatal(flow.connect<"out", "in">(ones, sink).has_value()));
            expect(fatal(sched.exchange(std::move(flow)).has_value()));
        }
        std::ignore = sched.runAndWait();
        expect(eq(sched.nStopsRefused, 0UZ)) << "the hook found the run stopped, or its stop was refused";
        expect(sched.state() == gr::lifecycle::State::STOPPED);
        expect(eq(sched.nStarts, 1UZ)) << "the start hook did not run once";
    };

    "BlockingSync reads sample_rate, chunk_size and use_internal_thread from a block's Settings"_test = [] {
        SettingsPacedSource source;
        expect(fatal(source.check({}).has_value()));
        expect(!source.isUsingInternalThread()) << "use_internal_thread from the Settings";
        source.blockingSyncStart();
        std::this_thread::sleep_for(std::chrono::milliseconds(20)); // 20000 samples at the sample rate, far above a chunk
        expect(eq(source.syncSamples(1000UZ), 7UZ)) << "a call is bounded by the chunk_size of the Settings";
        expect(fatal(source.set("chunk_size", gr::Size_t{3U}).has_value()));
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        expect(eq(source.syncSamples(1000UZ), 3UZ)) << "the chunk_size set since";
        expect(fatal(source.set("sample_rate", 0.001f).has_value()));
        source.blockingSyncResetTiming();
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        expect(eq(source.syncSamples(1000UZ), 0UZ)) << "at one sample in 1000 s no sample is due";
        source.blockingSyncStop();

        RateOnlySource rateOnly;
        expect(fatal(rateOnly.check({}).has_value()));
        expect(rateOnly.isUsingInternalThread()) << "no use_internal_thread: the timer thread";
        rateOnly.blockingSyncResetTiming();
        std::this_thread::sleep_for(std::chrono::milliseconds(300)); // 300 samples at the sample rate
        expect(eq(rateOnly.syncSamples(1000UZ), 100UZ)) << "no chunk_size: a tenth of the sample rate";
    };
};

} // namespace qa_settings_status
