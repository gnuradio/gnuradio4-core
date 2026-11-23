#include <boost/ut.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <complex>
#include <cstring>
#include <expected>
#include <format>
#include <limits>
#include <memory>
#include <mutex>
#include <numbers>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

#include <gnuradio-4.0/Block.hpp>
#include <gnuradio-4.0/BlockRegistry.hpp>
#include <gnuradio-4.0/Graph.hpp>
#include <gnuradio-4.0/Graph_yaml_importer.hpp>
#include <gnuradio-4.0/PluginLoader.hpp>
#include <gnuradio-4.0/Port.hpp>
#include <gnuradio-4.0/Scheduler.hpp>
#include <gnuradio-4.0/SchedulerModel.hpp>
#include <gnuradio-4.0/Settings.hpp>

#include "settings_utils.hpp"

namespace qa_settings_check {

using namespace boost::ut;
using gr::Annotated;
using gr::property_map;
using gr::settings::BlockSettingsOf;
using gr::settings::Rejected;
using gr::settings::Values;
using gr::settings::detail::BlockState;
using gr::settings::detail::FrameworkAccess;
using PmtValue = gr::pmt::Value;

// a de-emphasis filter: its check holds the rule on each setting's sign and the one rule that relates the two
struct FmDeemphasis : gr::Block<FmDeemphasis> {
    using Description = gr::Doc<"a one-pole de-emphasis of an FM audio stream">;

    gr::PortIn<float>  in;
    gr::PortOut<float> out;

    struct Settings {
        Annotated<float, gr::Unit<"Hz">, gr::Doc<"rate of the audio stream">>         sample_rate = 48000.f;
        Annotated<double, gr::Unit<"s">, gr::Doc<"emphasis time constant; 0 is off">> tau         = 75e-6;

        GR_MAKE_REFLECTABLE(Settings, sample_rate, tau);
    } settings;

    GR_MAKE_REFLECTABLE(FmDeemphasis, in, out, settings);

    double _feed    = 1.0;
    double _back    = 0.0;
    double _last    = 0.0;
    int    _designs = 0;

    [[nodiscard]] float processOne(float sample) noexcept {
        _last = _feed * static_cast<double>(sample) + _back * _last;
        return static_cast<float>(_last);
    }

    std::optional<gr::Refused> checkSettings(const Settings& s) const {
        if (s.sample_rate <= 0.f) {
            return gr::Refused(s.sample_rate, "not greater than 0");
        }
        if (s.tau < 0.0) {
            return gr::Refused(s.tau, "below 0");
        }
        if (s.tau == 0.0) {
            return {}; // emphasis off
        }
        const double corner = 1.0 / (2.0 * std::numbers::pi * s.tau);
        if (corner >= 0.5 * static_cast<double>(s.sample_rate)) {
            return gr::Refused(s.tau, std::format("the corner derived from tau ({} Hz) must lie below {} Hz", corner, 0.5 * static_cast<double>(s.sample_rate)));
        }
        return {};
    }

    void applySettings(const Settings& s) {
        ++_designs;
        if (s.tau.value == 0.0) {
            _feed = 1.0;
            _back = 0.0;
            return;
        }
        _back = std::exp(-1.0 / (static_cast<double>(s.sample_rate.value) * s.tau.value));
        _feed = 1.0 - _back;
    }
};

enum class Verdict { accept, emptyRefusal, unnamed, named, collected };

// refuses as its verdict says
struct Combination {
    struct Settings {
        int a = 1;
        int b = 2;

        GR_MAKE_REFLECTABLE(Settings, a, b);
    } settings;

    GR_MAKE_REFLECTABLE(Combination, settings);

    Verdict verdict = Verdict::accept;

    std::optional<gr::Refused> checkSettings(const Settings& s) const {
        switch (verdict) {
        case Verdict::accept: return {};
        case Verdict::emptyRefusal: return gr::Refused{};
        case Verdict::unnamed: return s.a + s.b > 10 ? std::optional(gr::Refused("a and b must sum to at most 10")) : std::nullopt;
        case Verdict::named: return gr::Refused(s.b, "b is refused");
        case Verdict::collected: {
            gr::Refused refused;
            refused.add(s.a, "a is refused");
            refused.add(s.b, "b is refused");
            return refused;
        }
        }
        return {};
    }
};

struct OutsideLimits {
    struct Settings {
        Annotated<int, gr::Limits<gr::Min<1>, gr::Max<10>>> order = 0;

        GR_MAKE_REFLECTABLE(Settings, order);
    } settings;

    GR_MAKE_REFLECTABLE(OutsideLimits, settings);
};

// floating-point settings with one predicate each, and one without limits
struct OneEndEach {
    struct Settings {
        Annotated<float, gr::Limits<gr::Over<0.f>>>    over      = 1.f;
        Annotated<double, gr::Limits<gr::Min<0.0>>>    min       = 1.0;
        Annotated<float, gr::Limits<gr::Max<10.f>>>    max       = 1.f;
        Annotated<double, gr::Limits<gr::Under<10.0>>> under     = 1.0;
        double                                         unbounded = 1.0;

        GR_MAKE_REFLECTABLE(Settings, over, min, max, under, unbounded);
    } settings;

    GR_MAKE_REFLECTABLE(OneEndEach, settings);
};

struct InfiniteDefault {
    struct Settings {
        Annotated<float, gr::Limits<gr::Min<0.f>>> gain = std::numeric_limits<float>::infinity();

        GR_MAKE_REFLECTABLE(Settings, gain);
    } settings;

    GR_MAKE_REFLECTABLE(InfiniteDefault, settings);
};

enum class Taper { hann, hamming, blackman };

struct Tapered {
    struct Settings {
        Taper taper = Taper::hamming;

        GR_MAKE_REFLECTABLE(Settings, taper);
    } settings;

    GR_MAKE_REFLECTABLE(Tapered, settings);
};

enum class Framing { plain, bit_stuffing, byte_escape };

struct Framer : gr::Block<Framer> {
    gr::PortIn<float> in;

    struct Settings {
        Framing framing = Framing::plain;

        GR_MAKE_REFLECTABLE(Settings, framing);
    } settings;

    GR_MAKE_REFLECTABLE(Framer, in, settings);

    explicit Framer(property_map init = {}) : gr::Block<Framer>(std::move(init)) {}

    gr::work::Status processBulk(gr::InputSpanLike auto& inSpan) {
        std::ignore = inSpan.consume(inSpan.size());
        return gr::work::Status::OK;
    }
};

// a source's samples and a pilot list with not-a-number among them, beside a gain; the apply hook records whether
// each list changed
struct NaNSamples {
    struct Settings {
        std::vector<float>               samples = {std::numeric_limits<float>::quiet_NaN(), 1.f};
        std::vector<std::complex<float>> pilots  = {{1.f, std::numeric_limits<float>::quiet_NaN()}};
        float                            gain    = 1.f;

        GR_MAKE_REFLECTABLE(Settings, samples, pilots, gain);
    } settings;

    GR_MAKE_REFLECTABLE(NaNSamples, settings);

    int  applies     = 0;
    bool samplesSeen = false;
    bool pilotsSeen  = false;

    // not-a-number compares unequal to itself, so the lists compare bit by bit
    template<typename T>
    [[nodiscard]] static bool sameBits(const std::vector<T>& left, const std::vector<T>& right) {
        return std::ranges::equal(left, right, [](const T& one, const T& other) { return std::memcmp(&one, &other, sizeof(T)) == 0; });
    }

    void applySettings(const Settings& s, const Settings& old) {
        ++applies;
        samplesSeen = !sameBits(s.samples, old.samples);
        pilotsSeen  = !sameBits(s.pilots, old.pilots);
    }
};

// a source of `n_samples` ones
struct Ones : gr::Block<Ones> {
    gr::PortOut<float> out;

    struct Settings {
        gr::Size_t n_samples = 16U;

        GR_MAKE_REFLECTABLE(Settings, n_samples);
    } settings;

    GR_MAKE_REFLECTABLE(Ones, out, settings);

    std::size_t      _sent = 0UZ;
    std::atomic<int> starts{0};

    void start() {
        ++starts;
        _sent = 0UZ;
    }

    gr::work::Status processBulk(gr::OutputSpanLike auto& outSpan) {
        const std::size_t n = std::min(outSpan.size(), static_cast<std::size_t>(settings.n_samples) - _sent);
        std::fill_n(outSpan.begin(), n, 1.f);
        outSpan.publish(n);
        _sent += n;
        return _sent >= settings.n_samples ? gr::work::Status::DONE : gr::work::Status::OK;
    }
};

struct Tally : gr::Block<Tally> {
    gr::PortIn<float> in;

    GR_MAKE_REFLECTABLE(Tally, in);

    std::atomic<std::size_t> received{0UZ};

    gr::work::Status processBulk(gr::InputSpanLike auto& inSpan) {
        received.fetch_add(inSpan.size(), std::memory_order_relaxed);
        std::ignore = inSpan.consume(inSpan.size());
        return gr::work::Status::OK;
    }
};

// what an apply hook saw of the change, and how often the block started
struct Hooks {
    std::atomic<int> applies{0};
    std::atomic<int> starts{0};
    bool             fromDefaults = false; // the latest apply's `old` held the declared defaults
};

// a convolutional encoder whose code is required and has no default: its declared defaults fail its own check
struct ConvEncode : gr::Block<ConvEncode> {
    gr::PortIn<float>  in;
    gr::PortOut<float> out;

    struct Settings {
        std::string code{};
        float       gain = 1.f;

        GR_MAKE_REFLECTABLE(Settings, code, gain);
    } settings;

    GR_MAKE_REFLECTABLE(ConvEncode, in, out, settings);

    Hooks hooks{};

    [[nodiscard]] constexpr float processOne(float sample) const noexcept { return sample * settings.gain; }

    void start() { ++hooks.starts; }

    std::optional<gr::Refused> checkSettings(const Settings& s) const {
        if (s.code.empty()) {
            return gr::Refused("a code name is required");
        }
        if (s.code != "voyager") {
            return gr::Refused(s.code, "the code names none of 'voyager'");
        }
        return {};
    }

    void applySettings([[maybe_unused]] const Settings& s, const Settings& old) {
        hooks.fromDefaults = old.code == Settings{}.code && old.gain == Settings{}.gain;
        ++hooks.applies;
    }
};

// a rate whose default is 0, which its own `Limits` refuse
struct ZeroRateBlock : gr::Block<ZeroRateBlock> {
    gr::PortIn<float>  in;
    gr::PortOut<float> out;

    struct Settings {
        Annotated<float, gr::Limits<gr::Min<1.f>, gr::Max<1e7f>>> sample_rate = 0.f;
        float                                                     gain        = 1.f;

        GR_MAKE_REFLECTABLE(Settings, sample_rate, gain);
    } settings;

    GR_MAKE_REFLECTABLE(ZeroRateBlock, in, out, settings);

    Hooks hooks{};

    [[nodiscard]] constexpr float processOne(float sample) const noexcept { return sample * settings.gain; }

    void start() { ++hooks.starts; }

    void applySettings([[maybe_unused]] const Settings& s, const Settings& old) {
        hooks.fromDefaults = old.sample_rate == Settings{}.sample_rate && old.gain == Settings{}.gain;
        ++hooks.applies;
    }
};

// a device whose state outside its settings may make it refuse a gain it accepted before
struct Strict : gr::Block<Strict> {
    gr::PortIn<float>  in;
    gr::PortOut<float> out;

    struct Settings {
        float gain = 1.f;

        GR_MAKE_REFLECTABLE(Settings, gain);
    } settings;

    GR_MAKE_REFLECTABLE(Strict, in, out, settings);

    Hooks hooks{};
    bool  refuseGain = false;

    [[nodiscard]] constexpr float processOne(float sample) const noexcept { return sample * settings.gain; }

    void start() { ++hooks.starts; }

    std::optional<gr::Refused> checkSettings(const Settings& s) const {
        if (refuseGain) {
            return gr::Refused(s.gain, "the device lost its gain stage");
        }
        return {};
    }

    void applySettings([[maybe_unused]] const Settings& s) { ++hooks.applies; }
};

// a source, the two blocks that are not ready and a sink, in a graph a scheduler holds
struct Chain {
    gr::scheduler::Simple<> scheduler;
    Ones*                   source = nullptr;
    ConvEncode*             conv   = nullptr;
    ZeroRateBlock*          rate   = nullptr;
    Tally*                  sink   = nullptr;

    Chain() {
        gr::Graph flow;
        source = std::addressof(flow.emplaceBlock<Ones>());
        conv   = std::addressof(flow.emplaceBlock<ConvEncode>());
        rate   = std::addressof(flow.emplaceBlock<ZeroRateBlock>());
        sink   = std::addressof(flow.emplaceBlock<Tally>());
        expect(fatal(flow.connect<"out", "in">(*source, *conv).has_value()));
        expect(fatal(flow.connect<"out", "in">(*conv, *rate).has_value()));
        expect(fatal(flow.connect<"out", "in">(*rate, *sink).has_value()));
        expect(fatal(scheduler.exchange(std::move(flow)).has_value()));
    }
};

constexpr std::string_view kDesignRefusal = "the cutoff must lie between 0 and half the sample rate";

// a library's lowpass design, which throws on a cutoff it cannot design
[[nodiscard]] std::vector<float> designLowpass(float cutoff, float sampleRate) {
    if (!(cutoff > 0.f && cutoff < sampleRate / 2.f)) {
        throw std::invalid_argument(std::string(kDesignRefusal));
    }
    return {cutoff / sampleRate, 1.f - cutoff / sampleRate};
}

// a lowpass whose check and apply hook both call the design, with the cutoff declared `kDefaultCutoff`
template<int kDefaultCutoff>
struct LibraryLowpass : gr::Block<LibraryLowpass<kDefaultCutoff>> {
    gr::PortIn<float>  in;
    gr::PortOut<float> out;

    struct Settings {
        float cutoff      = static_cast<float>(kDefaultCutoff);
        float sample_rate = 48000.f;

        GR_MAKE_REFLECTABLE(Settings, cutoff, sample_rate);
    } settings;

    GR_MAKE_REFLECTABLE(LibraryLowpass, in, out, settings);

    std::vector<float> taps{};
    Hooks              hooks{};

    [[nodiscard]] constexpr float processOne(float sample) const noexcept { return sample; }

    void start() { ++hooks.starts; }

    std::optional<gr::Refused> checkSettings(const Settings& s) const {
        std::ignore = designLowpass(s.cutoff, s.sample_rate);
        return {};
    }

    void applySettings(const Settings& s) {
        taps = designLowpass(s.cutoff, s.sample_rate);
        ++hooks.applies;
    }
};

using DesignedLowpass   = LibraryLowpass<1000>;
using UndesignedLowpass = LibraryLowpass<0>;

// a device that forwards its level downstream
struct Device : gr::Block<Device> {
    gr::PortIn<float>  in;
    gr::PortOut<float> out;

    struct Settings {
        float  level     = 1.f;
        double frequency = 100e6;

        GR_MAKE_REFLECTABLE(Settings, level, frequency);
    } settings;

    GR_MAKE_REFLECTABLE(Device, in, out, settings);

    std::atomic<int> applies{0};

    explicit Device(property_map init = {}) : gr::Block<Device>(std::move(init)) {}

    [[nodiscard]] constexpr float processOne(float sample) const noexcept { return sample * settings.level; }

    void applySettings(const Settings& s, const Settings& old) {
        if (s.level != old.level) {
            this->forwardTag("qa:level", s.level);
        }
        applies.fetch_add(1, std::memory_order_release);
    }
};

// a device that runs a level of 10 at most and has one antenna, which it reports by member from its apply hook
struct Bounded : gr::Block<Bounded> {
    gr::PortIn<float>  in;
    gr::PortOut<float> out;

    struct Settings {
        float       level   = 1.f;
        std::string antenna = "RX";

        GR_MAKE_REFLECTABLE(Settings, level, antenna);
    } settings;

    GR_MAKE_REFLECTABLE(Bounded, in, out, settings);

    [[nodiscard]] constexpr float processOne(float sample) const noexcept { return sample * settings.level; }

    void applySettings(const Settings& s, const Settings& old) {
        if (s.level != old.level && s.level > 10.f) {
            this->fail(s.level, "the device runs a level of 10 at most");
        }
        if (s.antenna != old.antenna && s.antenna != "RX") {
            this->fail(s.antenna, "the device has one antenna");
        }
    }
};

// a lowpass with no check whose design, in the apply hook, fails a cutoff of 0 or less
struct LowpassDesign : gr::Block<LowpassDesign> {
    gr::PortIn<float>  in;
    gr::PortOut<float> out;

    struct Settings {
        float cutoff = 1000.f;
        float gain   = 1.f;

        GR_MAKE_REFLECTABLE(Settings, cutoff, gain);
    } settings;

    GR_MAKE_REFLECTABLE(LowpassDesign, in, out, settings);

    std::atomic<int> designs{0};

    [[nodiscard]] constexpr float processOne(float sample) const noexcept { return sample * settings.gain; }

    void applySettings(const Settings& s) {
        if (s.cutoff <= 0.f) {
            this->fail(s.cutoff, "the design needs a cutoff above 0");
            return;
        }
        ++designs;
    }
};

struct Plain {
    struct Settings {
        int x = 0;

        GR_MAKE_REFLECTABLE(Settings, x);
    } settings;

    GR_MAKE_REFLECTABLE(Plain, settings);
};

struct WrongApply : Plain {
    void applySettings(int) {}
};

struct ConstApply : Plain {
    void applySettings() const {}
};

struct MutableCheck : Plain {
    std::optional<gr::Refused> checkSettings(const Settings&) { return {}; }
};

static_assert(gr::settings::HasSettingsCheck<FmDeemphasis> && gr::settings::HasSettingsApply<FmDeemphasis>);
static_assert(gr::settings::HasSettingsApply<Device>);
static_assert(gr::settings::detail::NamesApplySettings<WrongApply> && !gr::settings::HasSettingsApply<WrongApply>, "an apply hook of another form is refused");
static_assert(gr::settings::detail::NamesApplySettings<ConstApply> && !gr::settings::HasSettingsApply<ConstApply>, "a const apply hook is refused");
static_assert(gr::settings::detail::NamesCheckSettings<MutableCheck> && !gr::settings::HasSettingsCheck<MutableCheck>, "a check that may change the block is refused");

// a gain stage whose check takes the proposed values as a plain `Settings` and names a refused setting by the copy's
// member
struct Stage : gr::Block<Stage> {
    gr::PortIn<float>  in;
    gr::PortOut<float> out;

    struct Settings {
        Annotated<float, gr::Unit<"dB">, gr::Limits<gr::Min<-40.f>, gr::Max<40.f>>> gain = 0.f;
        Annotated<std::string, gr::Doc<"'linear' or 'clip'">>                       mode = std::string("linear");

        GR_MAKE_REFLECTABLE(Settings, gain, mode);
    } settings;

    GR_MAKE_REFLECTABLE(Stage, in, out, settings);

    int   _applies = 0;
    float _linear  = 1.f;

    explicit Stage(property_map init = {}) : gr::Block<Stage>(std::move(init)) {}

    [[nodiscard]] constexpr float processOne(float sample) const noexcept { return sample * _linear; }

    std::optional<gr::Refused> checkSettings(const Settings& s) const {
        if (s.mode.value == "linear" && s.gain.value > 20.f) {
            return gr::Refused(s.gain, "above 20 dB in linear mode");
        }
        return {};
    }

    void applySettings(const Settings& s) {
        ++_applies;
        _linear = std::pow(10.f, s.gain.value / 20.f);
    }
};

// what a stage holds after a scenario
struct StageSeen {
    bool                     accepted = false;
    float                    gain     = 0.f;
    std::string              mode{};
    int                      applies = 0;
    std::vector<std::string> refusedKeys{};
    std::string              reasons{};

    bool operator==(const StageSeen&) const = default;
};

[[nodiscard]] StageSeen seenAfter(Stage& stage, const std::expected<Values, Rejected>& result) {
    StageSeen seen{.accepted = result.has_value(), .gain = stage.settings.gain.value, .mode = stage.settings.mode.value, .applies = stage._applies};
    if (!result) {
        for (const gr::settings::Rejection& entry : result.error()) {
            seen.refusedKeys.push_back(entry.key);
        }
        seen.reasons = gr::settings::describeRejected(result.error());
    }
    return seen;
}

[[nodiscard]] StageSeen stageAfterPendingChange() {
    Stage stage;
    expect(stage.check({}).has_value());
    FrameworkAccess::moveState(gr::settings::detail::settingsOf(stage), BlockState::running);
    const std::expected<Values, Rejected> first = stage.set(property_map{{"mode", std::string("clip")}}, gr::settings::noWait);
    expect(first.has_value() && stage.settings.mode.value == "linear") << "the first call waits for the block's apply";
    const std::expected<Values, Rejected> second = stage.set(property_map{{"gain", 30.f}}, gr::settings::noWait);
    std::ignore                                  = gr::testing::applyStagedForTest(gr::settings::detail::settingsOf(stage));
    FrameworkAccess::moveState(gr::settings::detail::settingsOf(stage), BlockState::stopped);
    return seenAfter(stage, second);
}

template<typename TBlock>
struct Driven : BlockSettingsOf<TBlock> {
    using BlockSettingsOf<TBlock>::BlockSettingsOf;
    using BlockSettingsOf<TBlock>::publish;
};

template<typename TBlock>
struct Fixture {
    TBlock         block{};
    Driven<TBlock> settings;

    explicit Fixture(property_map constructedWith = {}) : settings(block, std::move(constructedWith)) {}
};

// a scheduler as a block's settings see it, at level info, holding each notification
struct RecordingScheduler : gr::settings::detail::SchedulerLink {
    mutable std::mutex                      lock;
    std::vector<gr::settings::Notification> held{};

    void push(gr::settings::Notification notification) override {
        std::lock_guard guard(lock);
        held.push_back(std::move(notification));
    }
    void                                           wake() override {}
    bool                                           isWorkerThread() const override { return false; }
    std::optional<gr::settings::NotificationLevel> level() const override { return gr::settings::NotificationLevel::info; }
    std::chrono::milliseconds                      defaultTimeout() const override { return std::chrono::milliseconds(1000); }

    [[nodiscard]] std::size_t publishedNotices(std::string_view key) const {
        std::lock_guard guard(lock);
        return static_cast<std::size_t>(std::ranges::count_if(held, [key](const gr::settings::Notification& entry) { return entry.key == key && entry.what == gr::settings::NotificationKind::changed; }));
    }
};

[[nodiscard]] std::vector<std::string> keysOf(const Rejected& refused) {
    std::vector<std::string> keys;
    for (const gr::settings::Rejection& entry : refused) {
        keys.push_back(entry.key);
    }
    std::ranges::sort(keys);
    return keys;
}

[[nodiscard]] std::string reasonsOf(const Rejected& refused) { return gr::settings::describeRejected(refused); }

const suite<"SettingsCheck: the hooks"> hookTests = [] {
    "a block designs at its setup and at each accepted change"_test = [] {
        FmDeemphasis block;
        expect(block.check({}).has_value());
        expect(eq(block._designs, 1));
        expect(block._back > 0.0 && block._back < 1.0);
        expect(block.set(property_map{{"sample_rate", 8000.f}}).has_value());
        expect(eq(block._designs, 2));
        expect(block.set(property_map{{"tau", 0.0}}).has_value());
        expect(eq(block._designs, 3));
        expect(eq(block.processOne(0.5f), 0.5f)) << "a tau of zero is off";
    };

    "a refusal by member names the setting, and the apply hook does not run"_test = [] {
        FmDeemphasis block;
        expect(block.check({}).has_value());
        const auto refused = block.set(property_map{{"tau", 1e-6}});
        expect(fatal(!refused.has_value()));
        expect(keysOf(refused.error()) == std::vector<std::string>{"tau"});
        expect(reasonsOf(refused.error()).contains("must lie below 24000 Hz")) << reasonsOf(refused.error());
        const auto zeroRate = block.set(property_map{{"sample_rate", 0.f}});
        expect(fatal(!zeroRate.has_value()));
        expect(reasonsOf(zeroRate.error()).contains("not greater than 0")) << reasonsOf(zeroRate.error());
        expect(eq(block._designs, 1));
        expect(eq(block.settings.tau.value, 75e-6));
    };
};

const suite<"SettingsCheck: refusals"> refusalTests = [] {
    "a refusal with no entry refuses nothing"_test = [] {
        Fixture<Combination> fixture;
        fixture.block.verdict = Verdict::emptyRefusal;
        expect(fixture.settings.set(property_map{{"a", 3}}).has_value());
        expect(eq(fixture.block.settings.a, 3));
    };

    "add() collects one entry per setting"_test = [] {
        Fixture<Combination> fixture;
        expect(fixture.settings.check({}).has_value());
        fixture.block.verdict = Verdict::collected;
        const auto refused    = fixture.settings.set(property_map{{"a", 3}});
        expect(fatal(!refused.has_value()));
        expect(keysOf(refused.error()) == std::vector<std::string>{"a", "b"});
    };

    "a refusal that names no setting is reported against each setting a set changes"_test = [] {
        Fixture<Combination> fixture;
        expect(fixture.settings.check({}).has_value());
        fixture.block.verdict = Verdict::unnamed;
        const auto one        = fixture.settings.set(property_map{{"a", 9}, {"b", 2}});
        expect(fatal(!one.has_value()));
        expect(keysOf(one.error()) == std::vector<std::string>{"a"}) << "b is sent at the value it holds";
        const auto both = fixture.settings.set(property_map{{"a", 6}, {"b", 6}});
        expect(fatal(!both.has_value()));
        expect(keysOf(both.error()) == std::vector<std::string>{"a", "b"});
        expect(fixture.settings.setTagCouplings(property_map{{"a", std::string("qa:a")}}).has_value());
        expect(keysOf(FrameworkAccess::tagEnters(fixture.settings, property_map{{"qa:a", 9}})) == std::vector<std::string>{"a"}) << "by a tag";
    };

    "a refusal that names no setting stays against the block at the setup and at a start"_test = [] {
        Fixture<Combination> fixture(property_map{{"a", 9}});
        fixture.block.verdict = Verdict::unnamed;
        const auto atSetUp    = fixture.settings.check({});
        expect(fatal(!atSetUp.has_value()));
        expect(keysOf(atSetUp.error()) == std::vector<std::string>{""});

        Fixture<Combination> started(property_map{{"a", 9}});
        expect(started.settings.check({}).has_value());
        started.block.verdict  = Verdict::unnamed;
        const Rejected refused = FrameworkAccess::beginRun(started.settings, true);
        expect(keysOf(refused) == std::vector<std::string>{""});
    };
};

const suite<"SettingsCheck: numbers and their bounds"> numberTests = [] {
    "a list holding not-a-number is unchanged when set to its own content, and changed when one element differs"_test = [] {
        constexpr float     notANumber = std::numeric_limits<float>::quiet_NaN();
        Fixture<NaNSamples> fixture;
        RecordingScheduler  scheduler;
        FrameworkAccess::attachScheduler(fixture.settings, &scheduler);
        expect(fixture.settings.check({}).has_value());
        expect(fixture.settings.set(property_map{{"gain", 2.f}}).has_value());
        expect(!fixture.block.samplesSeen) << "a set of another setting leaves the list out of the change";
        expect(!fixture.block.pilotsSeen) << "a list of complex numbers alike";
        const std::size_t before  = scheduler.publishedNotices("samples");
        const int         applies = fixture.block.applies;
        expect(fixture.settings.set(property_map{{"samples", std::vector<float>{notANumber, 1.f}}}).has_value());
        expect(eq(scheduler.publishedNotices("samples"), before)) << "the same content is no change";
        expect(eq(fixture.block.applies, applies)) << "and runs no apply hook";
        expect(fixture.settings.set(property_map{{"samples", std::vector<float>{notANumber, 2.f}}}).has_value());
        expect(eq(scheduler.publishedNotices("samples"), before + 1UZ)) << "one finite element differs";
        expect(fixture.block.samplesSeen);
        FrameworkAccess::detachScheduler(fixture.settings, &scheduler);
    };

    "a declared default its own range refuses holds the block not ready"_test = [] {
        Fixture<OutsideLimits> outside;
        const auto             limits = outside.settings.ready();
        expect(fatal(!limits.has_value())) << "a default outside its Limits is tested like any value";
        expect(keysOf(limits.error()) == std::vector<std::string>{"order"});
        expect(eq(reasonsOf(limits.error()), std::string("order: default out of range [1, 10]: 0")));

        Fixture<OutsideLimits> given(property_map{{"order", 2}});
        expect(given.settings.ready().has_value()) << "a constructed value replaces the default";

        Fixture<InfiniteDefault> infinite;
        const auto               notFinite = infinite.settings.ready();
        expect(fatal(!notFinite.has_value()));
        expect(eq(reasonsOf(notFinite.error()), std::string("gain: default not finite")));
    };

    "each limit refuses positive and negative infinity, and a setting without limits takes both"_test = [] {
        Fixture<OneEndEach> fixture;
        expect(fatal(fixture.settings.check({}).has_value()));
        std::vector<std::string> taken;
        for (const char* key : {"over", "min", "max", "under"}) {
            for (const double infinity : {std::numeric_limits<double>::infinity(), -std::numeric_limits<double>::infinity()}) {
                const std::expected<Values, Rejected> refused = fixture.settings.set(property_map{{key, infinity}});
                if (refused.has_value() || reasonsOf(refused.error()) != std::format("{}: not finite", key)) {
                    taken.push_back(std::format("{} = {}", key, infinity));
                }
            }
        }
        expect(taken.empty()) << "taken or refused with another text: " << taken.size();
        expect(eq(fixture.block.settings.over.value, 1.f) && eq(fixture.block.settings.min.value, 1.0) && eq(fixture.block.settings.max.value, 1.f) && eq(fixture.block.settings.under.value, 1.0));
        for (const double infinity : {std::numeric_limits<double>::infinity(), -std::numeric_limits<double>::infinity()}) {
            const std::expected<Values, Rejected> accepted = fixture.settings.set(property_map{{"unbounded", infinity}});
            expect(accepted.has_value() && accepted->empty()) << "a setting without limits refused an infinity";
            expect(eq(fixture.block.settings.unbounded, infinity));
        }
    };
};

const suite<"SettingsCheck: an enumeration's name"> enumerationTests = [] {
    "an enumerator's name matches as spelled and reads back so"_test = [] {
        Framer block;
        expect(block.check({}).has_value());
        const auto set = block.set(property_map{{"framing", std::string("bit_stuffing")}});
        expect(set.has_value()) << (set ? std::string{} : reasonsOf(set.error()));
        expect(block.settings.framing == Framing::bit_stuffing);
        expect(block.get<std::string>("framing") == std::string("bit_stuffing"));

        Framer atSetUp{property_map{{"framing", std::string("byte_escape")}}};
        expect(atSetUp.check({}).has_value());
        expect(atSetUp.settings.framing == Framing::byte_escape);
    };

    "a name in another letter case or without `_` is refused, listing the names"_test = [] {
        for (const std::string_view sent : {"BitStuffing", "BIT_STUFFING", "bitstuffing", "Bit_Stuffing", "bit_stuff"}) {
            Framer block;
            expect(block.check({}).has_value());
            const auto refused = block.set(property_map{{"framing", std::string(sent)}});
            expect(fatal(!refused.has_value())) << sent;
            expect(keysOf(refused.error()) == std::vector<std::string>{"framing"}) << sent;
            expect(eq(refused.error().front().reason, std::format("not one of: plain, bit_stuffing, byte_escape: {}", sent))) << sent;
            expect(block.settings.framing == Framing::plain) << sent;
        }
        Fixture<Tapered> atSetUp(property_map{{"taper", std::string("Hann")}});
        expect(!atSetUp.settings.check({}).has_value()) << "a constructed name in another letter case";

        Framer tagged;
        expect(tagged.check({}).has_value() && tagged.setTagCouplings(property_map{{"framing", std::string("qa:framing")}}).has_value());
        const Rejected byTag = FrameworkAccess::tagEnters(gr::settings::detail::settingsOf(tagged), property_map{{"qa:framing", std::string("bit_stuff")}});
        expect(eq(gr::settings::describeRejected(byTag), std::string("framing: not one of: plain, bit_stuffing, byte_escape: bit_stuff"))) << "by a tag";
    };

#ifdef GR_ENABLE_BLOCK_REGISTRY
#endif

#ifdef GR_ENABLE_BLOCK_REGISTRY
    "a graph file that names no enumerator is refused with the same text"_test = [] {
        gr::BlockRegistry     registry;
        gr::SchedulerRegistry schedulers;
        expect(fatal(registry.insert<Framer>("=qa::Framer")));
        gr::PluginLoader loader(registry, schedulers, std::vector<std::string>{});

        const std::string document = R"yaml(blocks:
  - id: qa::Framer
    parameters:
      name: framer
      framing: bit_stuff
)yaml";

        std::string reported;
        try {
            std::ignore = gr::loadGrc(loader, document);
        } catch (const gr::exception& e) {
            reported = e.message;
        }
        expect(reported.contains("framing: not one of: plain, bit_stuffing, byte_escape")) << reported;
    };
#endif
};

const suite<"SettingsCheck: the block's calls"> callTests = [] {
    "forwardTag from the apply hook sends the tag, and nothing at the setup"_test = [] {
        Device block{property_map{{"level", 2.f}}};
        expect(block.check({}).has_value());
        expect(FrameworkAccess::takeForwarded(gr::settings::detail::settingsOf(block)).empty()) << "the setup's forwards are dropped";
        expect(block.set(property_map{{"level", 3.f}}).has_value());
        const property_map forwarded = FrameworkAccess::takeForwarded(gr::settings::detail::settingsOf(block));
        expect(fatal(forwarded.contains("qa:level")));
        expect(forwarded.at("qa:level") == PmtValue(3.f));
    };

    "fail takes the member beside the text, and each failed key keeps its own reason"_test = [] {
        Bounded block;
        expect(block.check({}).has_value());
        const auto result = block.set(property_map{{"level", 30.f}, {"antenna", std::string("TX")}});
        expect(fatal(result.has_value()));
        expect(fatal(result->contains("level") && result->contains("antenna")));
        expect(result->at("level").state == gr::settings::Value::State::failed);
        expect(eq(result->at("level").reason, std::string("the device runs a level of 10 at most")));
        expect(eq(block.get<float>("level").value_or(0.f), 1.f)) << "the batch goes back";
        expect(result->at("antenna").state == gr::settings::Value::State::failed);
        expect(eq(result->at("antenna").reason, std::string("the device has one antenna")));
    };

    "a publish of the value the key holds announces nothing on the block's own thread"_test = [] {
        Device             block;
        RecordingScheduler scheduler;
        FrameworkAccess::attachScheduler(gr::settings::detail::settingsOf(block), &scheduler);
        expect(block.check({}).has_value());
        const std::size_t before = scheduler.publishedNotices("level");
        {
            const gr::settings::detail::OwnThread workCall(gr::settings::detail::settingsOf(block));
            FrameworkAccess::publish(gr::settings::detail::settingsOf(block), "level", PmtValue(4.f));
            expect(eq(scheduler.publishedNotices("level"), before + 1UZ));
            FrameworkAccess::publish(gr::settings::detail::settingsOf(block), "level", PmtValue(4.f));
            expect(eq(scheduler.publishedNotices("level"), before + 1UZ)) << "the same value again is no change";
        }
        FrameworkAccess::publish(gr::settings::detail::settingsOf(block), "level", PmtValue(4.f));
        expect(eq(scheduler.publishedNotices("level"), before + 2UZ)) << "another thread's report is announced";
        FrameworkAccess::detachScheduler(gr::settings::detail::settingsOf(block), &scheduler);
    };
};

const suite<"SettingsCheck: a block that is not ready"> notReadyTests = [] {
    "a required code with no default and a rate declared 0 outside its Limits are each added to a graph and are not ready"_test = [] {
        gr::Graph      flow;
        ConvEncode&    conv = flow.emplaceBlock<ConvEncode>();
        ZeroRateBlock& rate = flow.emplaceBlock<ZeroRateBlock>();
        expect(eq(flow.blocks().size(), 2UZ));
        const auto convReady = conv.ready();
        expect(fatal(!convReady.has_value()));
        expect(keysOf(convReady.error()) == std::vector<std::string>{""});
        expect(reasonsOf(convReady.error()).contains("a code name is required")) << reasonsOf(convReady.error());
        const auto rateReady = rate.ready();
        expect(fatal(!rateReady.has_value()));
        expect(keysOf(rateReady.error()) == std::vector<std::string>{"sample_rate"});
        expect(eq(reasonsOf(rateReady.error()), std::string("sample_rate: default out of range [1, 1e+07]: 0")));
        expect(eq(conv.hooks.applies.load(), 0) && eq(rate.hooks.applies.load(), 0)) << "an apply hook ran on settings that are not valid";

        UndesignedLowpass lowpass;
        const auto        thrown = lowpass.ready();
        expect(fatal(!thrown.has_value()));
        expect(reasonsOf(thrown.error()).contains(kDesignRefusal)) << "defaults the design throws on: " << reasonsOf(thrown.error());
    };

    "a graph that holds blocks that are not ready does not start, names both, and starts no block"_test = [] {
        Chain                                chain;
        const std::expected<void, gr::Error> ran = chain.scheduler.runAndWait();
        expect(fatal(!ran.has_value())) << "a graph started with blocks that are not ready";
        const std::string& message = ran.error().message;
        expect(message.contains(std::format("{}: ", chain.conv->unique_name)) && message.contains("a code name is required")) << message;
        expect(message.contains(std::format("{}.sample_rate: ", chain.rate->unique_name))) << message;
        expect(keysOf(FrameworkAccess::runRefusals(gr::settings::detail::settingsOf(*chain.conv))) == std::vector<std::string>{""});
        expect(keysOf(FrameworkAccess::runRefusals(gr::settings::detail::settingsOf(*chain.rate))) == std::vector<std::string>{"sample_rate"});
        expect(eq(chain.conv->hooks.starts.load(), 0) && eq(chain.rate->hooks.starts.load(), 0));
        expect(eq(chain.source->starts.load(), 0)) << "a block of the graph started before the check of every block";
        expect(eq(chain.sink->received.load(), 0UZ));
    };

    "a block that is not ready inside a nested scheduler refuses the outer start, and no block starts inside or outside"_test = [] {
        gr::Graph   inside;
        Ones&       innerSource = inside.emplaceBlock<Ones>();
        ConvEncode& conv        = inside.emplaceBlock<ConvEncode>();
        Tally&      innerSink   = inside.emplaceBlock<Tally>();
        expect(fatal(inside.connect<"out", "in">(innerSource, conv).has_value()));
        expect(fatal(inside.connect<"out", "in">(conv, innerSink).has_value()));
        auto inner = std::make_shared<gr::SchedulerWrapper<gr::scheduler::Simple<>>>();
        inner->setGraph(std::move(inside));

        gr::Graph outer;
        Ones&     outerSource = outer.emplaceBlock<Ones>();
        Tally&    outerSink   = outer.emplaceBlock<Tally>();
        expect(fatal(outer.connect<"out", "in">(outerSource, outerSink).has_value()));
        outer.addBlock(gr::SchedulerModel::asBlockModelPtr(inner));
        gr::scheduler::Simple<> scheduler;
        expect(fatal(scheduler.exchange(std::move(outer)).has_value()));

        const std::expected<void, gr::Error> ran = scheduler.runAndWait();
        expect(fatal(!ran.has_value())) << "a graph started with a nested block that is not ready";
        const std::string& message = ran.error().message;
        expect(message.contains(std::format("{}: a code name is required", conv.unique_name))) << message;
        expect(eq(conv.hooks.starts.load(), 0) && eq(innerSource.starts.load(), 0)) << "a block inside the nested scheduler started";
        expect(eq(outerSource.starts.load(), 0)) << "a block outside the nested scheduler started";
        expect(eq(innerSink.received.load(), 0UZ) && eq(outerSink.received.load(), 0UZ));
    };

    "a start the check refuses leaves the refusing block's pending change unapplied"_test = [] {
        gr::Graph flow;
        Ones&     source = flow.emplaceBlock<Ones>();
        Strict&   strict = flow.emplaceBlock<Strict>();
        Tally&    sink   = flow.emplaceBlock<Tally>();
        expect(fatal(flow.connect<"out", "in">(source, strict).has_value()));
        expect(fatal(flow.connect<"out", "in">(strict, sink).has_value()));
        gr::scheduler::Simple<> scheduler;
        expect(fatal(scheduler.exchange(std::move(flow)).has_value()));
        expect(fatal(eq(strict.hooks.applies.load(), 1)));

        expect(fatal(strict.set(property_map{{"gain", 3.f}}).has_value()));
        expect(fatal(eq(strict.settings.gain, 1.f))) << "the change applied at the call, so it is not pending";
        strict.refuseGain                        = true;
        const std::expected<void, gr::Error> ran = scheduler.runAndWait();
        expect(fatal(!ran.has_value())) << "a block whose check refuses started";
        expect(ran.error().message.contains(std::format("{}.gain: the device lost its gain stage", strict.unique_name))) << ran.error().message;
        expect(eq(strict.settings.gain, 1.f)) << "the pending change reached the member";
        expect(eq(strict.hooks.applies.load(), 1)) << "the pending change reached the apply hook";
        expect(eq(strict.hooks.starts.load(), 0) && eq(source.starts.load(), 0));
    };

    "a set that makes the settings valid runs the apply hook once over the declared defaults, and the graph then starts"_test = [] {
        Chain chain;
        expect(!chain.conv->set(property_map{{"gain", 2.f}}).has_value()) << "a set that leaves the settings invalid";
        expect(fatal(chain.conv->set(property_map{{"code", std::string("voyager")}}).has_value()));
        expect(eq(chain.conv->hooks.applies.load(), 1) && chain.conv->hooks.fromDefaults) << "the hook did not see the declared defaults as the values before";
        expect(chain.conv->ready().has_value());
        expect(eq(chain.conv->settings.code, std::string("voyager")));

        expect(fatal(chain.rate->set(property_map{{"sample_rate", 48000.f}, {"gain", 2.f}}).has_value()));
        expect(eq(chain.rate->hooks.applies.load(), 1) && chain.rate->hooks.fromDefaults);
        expect(eq(chain.rate->settings.sample_rate.value, 48000.f) && eq(chain.rate->settings.gain, 2.f));

        const std::expected<void, gr::Error> ran = chain.scheduler.runAndWait();
        expect(fatal(ran.has_value())) << (ran ? std::string{} : ran.error().message);
        expect(eq(chain.conv->hooks.starts.load(), 1) && eq(chain.rate->hooks.starts.load(), 1));
        expect(eq(chain.sink->received.load(), 16UZ));
        expect(eq(chain.conv->hooks.applies.load(), 1) && eq(chain.rate->hooks.applies.load(), 1)) << "the start applied a setting again";

        ZeroRateBlock refusedGiven(property_map{{"sample_rate", -1.f}});
        expect(!refusedGiven.set(property_map{{"gain", 2.f}}).has_value()) << "a block in no graph: the given value is still refused";
    };

    "a value the apply hook refuses at the setup refuses the block in a graph, and alone each check returns it until a set replaces it"_test = [] {
        gr::Graph flow;
        expect(throws<gr::exception>([&flow] { std::ignore = flow.emplaceBlock<LowpassDesign>(property_map{{"cutoff", 0.f}}); }));
        expect(eq(flow.blocks().size(), 0UZ));

        LowpassDesign                       design(property_map{{"cutoff", 0.f}});
        const std::expected<void, Rejected> first = design.check({});
        expect(fatal(!first.has_value())) << "a check accepted the setup the apply hook refused";
        expect(keysOf(first.error()) == std::vector<std::string>{"cutoff"});
        expect(reasonsOf(first.error()).contains("the design needs a cutoff above 0")) << reasonsOf(first.error());
        const std::expected<void, Rejected> second = design.check({});
        expect(!second.has_value()) << "a second check accepted it";
        const std::expected<void, Rejected> keepsCutoff = design.check(property_map{{"gain", 2.f}});
        expect(!keepsCutoff.has_value()) << "a check that keeps the cutoff accepted it";
        const std::expected<void, Rejected> replacesCutoff = design.check(property_map{{"cutoff", 500.f}});
        expect(replacesCutoff.has_value()) << "a check that replaces the cutoff was refused";
        expect(eq(design.designs.load(), 0));

        expect(design.set(property_map{{"cutoff", 500.f}}).has_value());
        expect(eq(design.designs.load(), 1));
        expect(design.check({}).has_value());
    };

    "a work call on a block alone whose setup the apply hook refused returns ERROR"_test = [] {
        LowpassDesign      design(property_map{{"cutoff", 0.f}});
        gr::PortOut<float> upstream;
        gr::PortIn<float>  downstream;
        expect(fatal(upstream.connect(design.in).has_value()));
        expect(fatal(design.out.connect(downstream).has_value()));
        const gr::work::Result result = design.work();
        expect(result.status == gr::work::Status::ERROR) << "the block worked with a setup its apply hook refused";
        expect(eq(design.designs.load(), 0));
    };
};

const suite<"SettingsCheck: a check that throws"> throwingCheckTests = [] {
    "a set the design throws on is refused with its message under the setting, and the block keeps its value"_test = [] {
        DesignedLowpass lowpass;
        expect(fatal(lowpass.ready().has_value()));
        expect(fatal(eq(lowpass.hooks.applies.load(), 1)));
        const auto checked = lowpass.check(property_map{{"cutoff", 30000.f}});
        expect(fatal(!checked.has_value()));
        expect(keysOf(checked.error()) == std::vector<std::string>{"cutoff"});
        const auto refused = lowpass.set(property_map{{"cutoff", 30000.f}});
        expect(fatal(!refused.has_value())) << "the throw reached the caller as an accepted set";
        expect(keysOf(refused.error()) == std::vector<std::string>{"cutoff"});
        expect(reasonsOf(refused.error()).contains(kDesignRefusal)) << reasonsOf(refused.error());
        expect(eq(lowpass.settings.cutoff, 1000.f));
        expect(eq(lowpass.get<float>("cutoff").value_or(0.f), 1000.f)) << "a refused set left a value behind";
        expect(eq(lowpass.hooks.applies.load(), 1));
    };
};

const suite<"SettingsCheck: a block's settings in its struct"> structTests = [] {
    "a second call is judged over a first that is accepted and not yet applied"_test = [] {
        const StageSeen seen = stageAfterPendingChange();
        expect(seen.accepted && eq(seen.gain, 30.f) && seen.mode == "clip") << seen.reasons;
    };

#ifdef GR_ENABLE_BLOCK_REGISTRY
    "a graph file's values reach the struct's members"_test = [] {
        gr::BlockRegistry     registry;
        gr::SchedulerRegistry schedulers;
        expect(fatal(registry.insert<Stage>("=qa::Stage")));
        gr::PluginLoader loader(registry, schedulers, std::vector<std::string>{});

        const std::string document = R"yaml(blocks:
  - id: qa::Stage
    parameters:
      name: stage
      gain: 12
      mode: clip
)yaml";

        std::string reported;
        try {
            auto loaded = gr::loadGrc(loader, document);
            expect(fatal(eq(loaded->blocks().size(), 1UZ)));
            const auto* stage = static_cast<Stage*>(loaded->blocks().front()->raw());
            expect(eq(stage->settings.gain.value, 12.f) && stage->settings.mode.value == "clip");
        } catch (const gr::exception& e) {
            reported = e.message;
        }
        expect(reported.empty()) << reported;
    };
#endif
};

} // namespace qa_settings_check
