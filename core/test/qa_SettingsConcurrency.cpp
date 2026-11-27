#include <boost/ut.hpp>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <gnuradio-4.0/Block.hpp>
#include <gnuradio-4.0/Graph.hpp>
#include <gnuradio-4.0/Scheduler.hpp>
#include <gnuradio-4.0/Sequence.hpp>
#include <gnuradio-4.0/Settings.hpp>

#include "settings_utils.hpp"

namespace qa_settings {

struct TunableBlock : gr::Block<TunableBlock> {
    gr::PortIn<float>  in;
    gr::PortOut<float> out;

    struct Settings {
        gr::Annotated<float> gain        = 1.0f;
        gr::Annotated<float> sample_rate = 1000.0f;

        GR_MAKE_REFLECTABLE(Settings, gain, sample_rate);
    } settings;

    GR_MAKE_REFLECTABLE(TunableBlock, in, out, settings);

    [[nodiscard]] constexpr float processOne(float value) const noexcept { return value * settings.gain; }
};

// the common shape: the settings hook reads the settings back
struct ReentrantBlock : gr::Block<ReentrantBlock> {
    gr::PortIn<float>  in;
    gr::PortOut<float> out;

    struct Settings {
        gr::Annotated<float> gain = 1.0f;

        GR_MAKE_REFLECTABLE(Settings, gain);
    } settings;

    GR_MAKE_REFLECTABLE(ReentrantBlock, in, out, settings);

    std::size_t nCallbacks    = 0UZ;
    std::size_t nKeysObserved = 0UZ;

    [[nodiscard]] constexpr float processOne(float value) const noexcept { return value * settings.gain; }

    void applySettings([[maybe_unused]] const Settings& s) {
        nCallbacks++;
        nKeysObserved += this->get().value_or(gr::settings::Values{}).size();
    }
};

// once armed, the settings hook starts a device thread that publishes the level it runs, as a driver whose callback
// thread reports what a retune changed does; the publish waits for the hook, and the case joins the thread
struct PublishingBlock : gr::Block<PublishingBlock> {
    gr::PortIn<float>  in;
    gr::PortOut<float> out;

    struct Settings {
        gr::Annotated<float> gain  = 1.0f;
        gr::Annotated<float> level = 0.0f;

        GR_MAKE_REFLECTABLE(Settings, gain, level);
    } settings;

    GR_MAKE_REFLECTABLE(PublishingBlock, in, out, settings);

    std::optional<float> _levelToPublish;
    std::thread          _device;

    [[nodiscard]] constexpr float processOne(float value) const noexcept { return value * settings.gain; }

    void applySettings([[maybe_unused]] const Settings& s) {
        if (const std::optional<float> measured = std::exchange(_levelToPublish, std::nullopt); measured.has_value()) {
            _device = std::thread([this, value = *measured] { this->publish("level", value, "the device runs this level"); });
        }
    }
};

// the settings hook runs the call a case gives it on its own block, under the block lock its apply holds; the stop hook
// writes a status value, which its copy reports under that lock when the apply hook stops the block
struct HookCaller : gr::Block<HookCaller> {
    gr::PortIn<float>  in;
    gr::PortOut<float> out;

    struct Settings {
        gr::Annotated<float> gain  = 1.0f;
        gr::Annotated<float> level = 0.0f;

        GR_MAKE_REFLECTABLE(Settings, gain, level);
    } settings;

    struct Status {
        float reading = 0.0f;

        GR_MAKE_REFLECTABLE(Status, reading);
    } status;

    GR_MAKE_REFLECTABLE(HookCaller, in, out, settings, status);

    std::function<void(HookCaller&)> inApply{};
    std::atomic<int>                 nStops{0};

    [[nodiscard]] constexpr float processOne(float value) const noexcept { return value * settings.gain; }

    void applySettings([[maybe_unused]] const Settings& s) {
        if (inApply) {
            inApply(*this);
        }
    }

    void stop() {
        ++nStops;
        status.reading = 7.0f;
    }
};

// the settings hook runs the step a case gives it while another thread calls the block; no `Status`, whose copy belongs
// to the block's own thread, so the other thread's stop hook shares nothing with the apply
struct WaitingHook : gr::Block<WaitingHook> {
    gr::PortIn<float>  in;
    gr::PortOut<float> out;

    struct Settings {
        gr::Annotated<float> gain = 1.0f;

        GR_MAKE_REFLECTABLE(Settings, gain);
    } settings;

    GR_MAKE_REFLECTABLE(WaitingHook, in, out, settings);

    std::function<void()> inApply{};
    std::atomic<int>      nStops{0};

    [[nodiscard]] constexpr float processOne(float value) const noexcept { return value * settings.gain; }

    void applySettings([[maybe_unused]] const Settings& s) {
        if (inApply) {
            inApply();
        }
    }

    void stop() { ++nStops; }
};

// counts the settings hook's calls and runs `inApply` inside it; gain is limited to [0, 10]
struct CountingHook : gr::Block<CountingHook> {
    gr::PortIn<float>  in;
    gr::PortOut<float> out;

    struct Settings {
        gr::Annotated<float, gr::Limits<gr::Min<0.0f>, gr::Max<10.0f>>> gain  = 1.0f;
        gr::Annotated<float>                                            level = 0.0f;

        GR_MAKE_REFLECTABLE(Settings, gain, level);
    } settings;

    GR_MAKE_REFLECTABLE(CountingHook, in, out, settings);

    std::function<void()> inApply{};
    std::atomic<int>      nApplies{0};

    [[nodiscard]] constexpr float processOne(float value) const noexcept { return value * settings.gain; }

    void applySettings([[maybe_unused]] const Settings& s) {
        ++nApplies;
        if (inApply) {
            inApply();
        }
    }
};

// holds an apply hook that runs `hold()` until the case calls `release()`
struct HookHold {
    std::atomic<bool> inHook{false};
    std::atomic<bool> released{false};

    void hold() {
        inHook.store(true);
        inHook.notify_all();
        released.wait(false);
    }
    void waitForHook() const { inHook.wait(false); }
    void release() {
        released.store(true);
        released.notify_all();
    }
};

// whether `flag` rises within `bound`. A case expects it to stay down while a hook holds the block lock, which holds
// whatever the bound; the bound sets only how soon a call that does not wait shows
[[nodiscard]] bool risesWithin(const std::atomic<bool>& flag, std::chrono::milliseconds bound) {
    const auto deadline = std::chrono::steady_clock::now() + bound;
    while (!flag.load() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::yield();
    }
    return flag.load();
}

/// couples the block's sample_rate to gr:sample_rate
void coupleRate(gr::SettingsBase& settings) { std::ignore = settings.setTagCouplings({{"sample_rate", std::string(gr::tag::SAMPLE_RATE.key())}}); }

// a tag at the first sample of a work call, as the work call hands it to the settings
void enterTag(gr::SettingsBase& settings, const gr::property_map& tagMap) { std::ignore = gr::settings::detail::FrameworkAccess::tagEnters(settings, tagMap); }

// a block alone that runs, so that its changes wait for the thread that drives it
template<typename TBlock>
void startAlone(TBlock& block) {
    block.init(std::make_shared<gr::Sequence>());
    std::ignore = block.changeStateTo(gr::lifecycle::State::RUNNING);
}

template<typename TBlock>
void stopAlone(TBlock& block) {
    std::ignore = block.changeStateTo(gr::lifecycle::State::REQUESTED_STOP);
    std::ignore = block.changeStateTo(gr::lifecycle::State::STOPPED);
}

// gain is range-limited, so the check refuses an out-of-range value; the hook forwards every key the batch changed and
// throws for a gain above 5, which the limits let through
struct ValidatingBlock : gr::Block<ValidatingBlock> {
    gr::PortIn<float>  in;
    gr::PortOut<float> out;

    struct Settings {
        gr::Annotated<float, gr::Limits<gr::Min<0.0f>, gr::Max<10.0f>>> gain        = 1.0f;
        gr::Annotated<float>                                            sample_rate = 1000.0f;

        GR_MAKE_REFLECTABLE(Settings, gain, sample_rate);
    } settings;

    GR_MAKE_REFLECTABLE(ValidatingBlock, in, out, settings);

    [[nodiscard]] constexpr float processOne(float value) const noexcept { return value * settings.gain; }

    void applySettings(const Settings& s, const Settings& old) {
        if (s.gain != old.gain) {
            this->forwardTag("gain", s.gain.value);
        }
        if (s.sample_rate != old.sample_rate) {
            this->forwardTag("sample_rate", s.sample_rate.value);
        }
        if (s.gain != old.gain && s.gain > 5.0f) {
            throw gr::exception("the device takes no gain above 5");
        }
    }
};

// declares two default-tag keys with types other than the canonical float32 sample_rate and gr::Size_t
// num_channels, as an application using double rates does
struct WideRateBlock : gr::Block<WideRateBlock> {
    gr::PortIn<float>  in;
    gr::PortOut<float> out;

    struct Settings {
        gr::Annotated<double>       sample_rate  = 1.0;
        gr::Annotated<std::int64_t> num_channels = 1;

        GR_MAKE_REFLECTABLE(Settings, sample_rate, num_channels);
    } settings;

    GR_MAKE_REFLECTABLE(WideRateBlock, in, out, settings);

    [[nodiscard]] constexpr float processOne(float value) const noexcept { return value; }
};

struct NarrowRateSink : gr::Block<NarrowRateSink> {
    gr::PortIn<float> in;

    struct Settings {
        gr::Annotated<float> sample_rate = 1.0f;

        GR_MAKE_REFLECTABLE(Settings, sample_rate);
    } settings;

    GR_MAKE_REFLECTABLE(NarrowRateSink, in, settings);

    std::size_t nReceived = 0UZ;

    void processOne(float) { nReceived++; }
};

struct RateSource : gr::Block<RateSource> {
    gr::PortOut<float> out;

    struct Settings {
        gr::Annotated<float> sample_rate = 1.0f;

        GR_MAKE_REFLECTABLE(Settings, sample_rate);
    } settings;

    GR_MAKE_REFLECTABLE(RateSource, out, settings);

    static constexpr std::size_t kSamples = 4096UZ;

    std::size_t nProduced = 0UZ;

    // the rate the source is built with is no change, so the source announces it on its first sample itself
    float processOne() {
        if (nProduced == 0UZ) {
            this->publishTag(gr::property_map{{gr::tag::SAMPLE_RATE.key(), settings.sample_rate.value}}, 0UZ);
        }
        if (++nProduced >= kSamples) {
            this->requestStop();
        }
        return 1.0f;
    }
};

struct BadTagSource : gr::Block<BadTagSource> {
    gr::PortOut<float> out;

    GR_MAKE_REFLECTABLE(BadTagSource, out);

    static constexpr std::size_t kSamples = 4096UZ;

    std::size_t _nProduced = 0UZ;

    float processOne() {
        if (_nProduced == 0UZ) {
            this->publishTag(gr::property_map{{gr::tag::SAMPLE_RATE.key(), std::string("not-a-number")}}, 0UZ);
        }
        if (++_nProduced >= kSamples) {
            this->requestStop();
        }
        return 1.0f;
    }
};

} // namespace qa_settings

const boost::ut::suite<"settings concurrency"> settingsConcurrencyTests = [] {
    using namespace boost::ut;

    "concurrent two-key sets, applies and reads keep every call whole"_test = [] {
        constexpr std::size_t kIterations = 100UZ;
        constexpr float       kRateApart  = 1000.f; // each call sends a rate this far above its gain

        qa_settings::TunableBlock block;
        qa_settings::startAlone(block);

        // each read during the run holds the pair the block was built with or a pair one call sent
        const float              builtGain = block.settings.gain.value;
        const float              builtRate = block.settings.sample_rate.value;
        std::atomic<std::size_t> nTornReads{0UZ};
        const auto               sendPairs = [&block](std::size_t firstGain) {
            for (std::size_t i = 0UZ; i < kIterations; ++i) {
                const float gain = static_cast<float>(firstGain + i);
                std::ignore      = block.set({{"gain", gain}, {"sample_rate", gain + kRateApart}}, gr::settings::noWait);
            }
        };
        {
            std::vector<std::jthread> workers;
            workers.emplace_back(sendPairs, 0UZ);
            workers.emplace_back(sendPairs, kIterations);
            workers.emplace_back([&block] { // the one applying thread, as a worker is
                for (std::size_t i = 0UZ; i < kIterations; ++i) {
                    std::ignore = gr::testing::applyStagedForTest(gr::settings::detail::settingsOf(block));
                }
            });
            workers.emplace_back([&block, builtGain, builtRate, &nTornReads] {
                for (std::size_t i = 0UZ; i < kIterations; ++i) {
                    const gr::settings::Values all     = block.get().value_or(gr::settings::Values{});
                    const auto                 floatAt = [&all](std::string_view key) {
                        const auto it = all.find(key);
                        return it != all.end() ? it->second.as<float>().value_or(-1.f) : -1.f;
                    };
                    const float gain = floatAt("gain");
                    const float rate = floatAt("sample_rate");
                    if ((gain != builtGain || rate != builtRate) && rate != gain + kRateApart) {
                        nTornReads.fetch_add(1UZ, std::memory_order_relaxed);
                    }
                }
            });
        }
        std::ignore = gr::testing::applyStagedForTest(gr::settings::detail::settingsOf(block));

        expect(eq(nTornReads.load(), 0UZ)) << "a read during the run holds a gain and a rate no one call sent together";
        const float lastGain = block.settings.gain.value;
        expect(lastGain == static_cast<float>(kIterations - 1UZ) || lastGain == static_cast<float>(2UZ * kIterations - 1UZ)) << "the last gain of neither sender is in force";
        expect(eq(block.settings.sample_rate.value, lastGain + kRateApart)) << "the rate in force is not the one sent with the gain";
        expect(block.get<float>("gain") == lastGain) << "get() reads another gain than the field holds";
        qa_settings::stopAlone(block);
    };

    "a value a device thread publishes during the settings hook is read after the hook beside the value last set"_test = [] {
        qa_settings::PublishingBlock block;
        qa_settings::startAlone(block);
        block._levelToPublish = 3.0f;

        expect(block.set({{"gain", 2.0f}}, gr::settings::noWait).has_value());
        std::ignore = gr::testing::applyStagedForTest(gr::settings::detail::settingsOf(block));
        expect(fatal(block._device.joinable())) << "the hook started no device thread";
        block._device.join();
        expect(block.get<float>("level") == 3.0f) << "get() reads a level other than the one published";
        expect(eq(block.settings.level.value, 0.0f)) << "the published level replaced the value last set";
        qa_settings::stopAlone(block);
    };

    "a settings hook may read the settings back"_test = [] {
        qa_settings::ReentrantBlock block;
        block.init(std::make_shared<gr::Sequence>());

        expect(block.set({{"gain", 2.0f}}).has_value());

        expect(eq(block.nCallbacks, 2UZ)) << "the hook runs at the initialization and at the call";
        expect(gt(block.nKeysObserved, 0UZ)) << "the hook could not read the settings back";
        expect(eq(block.settings.gain.value, 2.0f)) << "the value was not applied";
    };

    "six no-wait sets during the settings hook return pending at once, and the sixth applies at the next apply"_test = [] {
        qa_settings::CountingHook block;
        qa_settings::startAlone(block);
        const int             appliesBefore = block.nApplies.load();
        qa_settings::HookHold hold;
        block.inApply = [&hold] { hold.hold(); };

        expect(block.set({{"gain", 1.5f}}, gr::settings::noWait).has_value());
        std::thread worker([&block] { std::ignore = gr::testing::applyStagedForTest(gr::settings::detail::settingsOf(block)); });
        hold.waitForHook();
        // a set that waited for the block lock would wait for the release below, which comes after the last of them
        std::vector<std::expected<gr::settings::Values, gr::settings::Rejected>> results;
        for (int gain = 2; gain <= 7; ++gain) {
            results.push_back(block.set({{"gain", static_cast<float>(gain)}}, gr::settings::noWait));
        }
        hold.release();
        worker.join();
        block.inApply = {};

        for (const std::expected<gr::settings::Values, gr::settings::Rejected>& result : results) {
            expect(fatal(result.has_value())) << "a set during the hook was refused";
            expect(eq(result->at("gain").reason, std::string(gr::settings::kPendingQueued)));
        }
        expect(eq(block.settings.gain.value, 1.5f)) << "the apply under way took a value set during its hook";
        std::ignore = gr::testing::applyStagedForTest(gr::settings::detail::settingsOf(block));
        expect(eq(block.settings.gain.value, 7.0f)) << "the sixth value is not in force";
        expect(eq(block.nApplies.load() - appliesBefore, 2)) << "the five earlier values were applied";
        qa_settings::stopAlone(block);
    };

    "a no-wait set during the settings hook refuses an undeclared key at once, and a value the check refuses later is a failed notification"_test = [] {
        qa_settings::CountingHook block;
        qa_settings::startAlone(block);
        qa_settings::HookHold hold;
        block.inApply = [&hold] { hold.hold(); };

        expect(block.set({{"gain", 2.0f}}, gr::settings::noWait).has_value());
        std::thread worker([&block] { std::ignore = gr::testing::applyStagedForTest(gr::settings::detail::settingsOf(block)); });
        hold.waitForHook();
        const std::expected<gr::settings::Values, gr::settings::Rejected> undeclared = block.set({{"no_such_key", 1.0f}}, gr::settings::noWait);
        const std::expected<gr::settings::Values, gr::settings::Rejected> outOfRange = block.set({{"gain", 20.0f}}, gr::settings::noWait);
        hold.release();
        worker.join();
        block.inApply = {};

        expect(fatal(!undeclared.has_value())) << "the undeclared key was taken";
        expect(eq(undeclared.error().front().reason, std::string("unknown setting")));
        expect(fatal(outOfRange.has_value())) << "the slotted value was refused before its check";
        expect(eq(outOfRange->at("gain").reason, std::string(gr::settings::kPendingQueued)));
        const int                appliesBefore = block.nApplies.load();
        gr::testing::CerrCapture captured;
        std::ignore = gr::testing::applyStagedForTest(gr::settings::detail::settingsOf(block));
        expect(eq(captured.lines("out of range"), 1UZ)) << captured.str();
        expect(eq(block.settings.gain.value, 2.0f)) << "the refused value replaced the value in force";
        expect(eq(block.nApplies.load(), appliesBefore)) << "the refused value reached the hook";
        qa_settings::stopAlone(block);
    };

    "a refused value is neither applied nor forwarded"_test = [] {
        qa_settings::ValidatingBlock block;
        block.init(std::make_shared<gr::Sequence>());

        const auto refused = block.set({{"gain", 99.0f}, {"sample_rate", 48000.0f}});
        expect(!refused.has_value() && refused.error().front().key == "gain") << "the limits did not refuse the gain";
        expect(eq(block.settings.gain.value, 1.0f)) << "the out-of-range value reached the block";
        expect(eq(block.settings.sample_rate.value, 1000.0f)) << "a call the check refuses applied another of its keys";

        const auto failed = block.set({{"gain", 7.0f}, {"sample_rate", 48000.0f}});
        expect(failed.has_value() && failed->contains("gain") && failed->at("gain").isFailed()) << "the hook's throw failed no key";
        expect(eq(block.settings.gain.value, 1.0f)) << "the value the hook refused stayed in the field";
        expect(gr::settings::detail::FrameworkAccess::takeForwarded(gr::settings::detail::settingsOf(block)).empty()) << "a batch the hook threw for forwarded a setting";

        expect(block.set({{"sample_rate", 44100.0f}}).has_value());
        const gr::property_map forwarded = gr::settings::detail::FrameworkAccess::takeForwarded(gr::settings::detail::settingsOf(block));
        expect(forwarded.contains("sample_rate") && !forwarded.contains("gain")) << "an applied key was not forwarded, or a refused one was";
        expect(eq(block.settings.sample_rate.value, 44100.0f)) << "a valid value was not applied";
    };

    "a default-tag value of another numeric type is converted to the member's type"_test = [] {
        qa_settings::WideRateBlock block;
        block.init(std::make_shared<gr::Sequence>());
        expect(block.setTagCouplings({{"sample_rate", std::string(gr::tag::SAMPLE_RATE.key())}, {"num_channels", std::string(gr::tag::NUM_CHANNELS.key())}}).has_value());

        qa_settings::enterTag(gr::settings::detail::settingsOf(block), {{gr::tag::SAMPLE_RATE.key(), 2.4e6f}, {gr::tag::NUM_CHANNELS.key(), 4U}});
        const gr::settings::Rejected failed = gr::testing::applyStagedForTest(gr::settings::detail::settingsOf(block));

        expect(failed.empty()) << "a convertible numeric value failed";
        expect(eq(block.settings.sample_rate.value, 2.4e6)) << "the float32 tag did not reach the double member";
        expect(eq(block.settings.num_channels.value, std::int64_t(4))) << "the gr::Size_t tag did not reach the int64 member";
        expect(gr::settings::detail::FrameworkAccess::takeForwarded(gr::settings::detail::settingsOf(block)).empty()) << "a tag's value was forwarded as a second tag";
    };

    "a wider default-tag value is narrowed onto the canonical member type"_test = [] {
        qa_settings::NarrowRateSink block;
        block.init(std::make_shared<gr::Sequence>());
        qa_settings::coupleRate(gr::settings::detail::settingsOf(block));

        qa_settings::enterTag(gr::settings::detail::settingsOf(block), {{gr::tag::SAMPLE_RATE.key(), 2.4e6}});
        std::ignore = gr::testing::applyStagedForTest(gr::settings::detail::settingsOf(block));

        expect(eq(block.settings.sample_rate.value, 2.4e6f)) << "the float64 tag did not reach the float member";
    };

    "a default-tag value outside the member's range is not applied"_test = [] {
        qa_settings::NarrowRateSink block;
        block.init(std::make_shared<gr::Sequence>());
        qa_settings::coupleRate(gr::settings::detail::settingsOf(block));

        qa_settings::enterTag(gr::settings::detail::settingsOf(block), {{gr::tag::SAMPLE_RATE.key(), 1e300}});
        std::ignore = gr::testing::applyStagedForTest(gr::settings::detail::settingsOf(block));

        expect(eq(block.settings.sample_rate.value, 1.0f)) << "an out-of-range value was applied instead of range-checked";
    };

    "a non-numeric default-tag value is not applied"_test = [] {
        qa_settings::WideRateBlock block;
        block.init(std::make_shared<gr::Sequence>());
        qa_settings::coupleRate(gr::settings::detail::settingsOf(block));

        qa_settings::enterTag(gr::settings::detail::settingsOf(block), {{gr::tag::SAMPLE_RATE.key(), std::string("not-a-number")}});
        std::ignore = gr::testing::applyStagedForTest(gr::settings::detail::settingsOf(block));

        expect(eq(block.settings.sample_rate.value, 1.0)) << "a string was coerced into a numeric member";
    };

    "a float32 rate tag crosses a double-typed block and reaches a float sink"_test = [] {
        gr::scheduler::Simple<>      scheduler;
        qa_settings::WideRateBlock*  wideBlock = nullptr;
        qa_settings::NarrowRateSink* sink      = nullptr;
        {
            gr::Graph flow;
            auto&     source = flow.emplaceBlock<qa_settings::RateSource>({{"sample_rate", 2.4e6f}});
            wideBlock        = &flow.emplaceBlock<qa_settings::WideRateBlock>();
            sink             = &flow.emplaceBlock<qa_settings::NarrowRateSink>();
            qa_settings::coupleRate(gr::settings::detail::settingsOf(*wideBlock));
            qa_settings::coupleRate(gr::settings::detail::settingsOf(*sink));
            expect(flow.connect<"out", "in">(source, *wideBlock).has_value());
            expect(flow.connect<"out", "in">(*wideBlock, *sink).has_value());
            expect(scheduler.exchange(std::move(flow)).has_value());
        }

        expect(scheduler.runAndWait().has_value()) << "the graph did not run to completion";
        expect(gt(sink->nReceived, 0UZ)) << "the sink received nothing";
        expect(eq(wideBlock->settings.sample_rate.value, 2.4e6)) << "the float32 rate tag did not reach the double member";
        expect(eq(sink->settings.sample_rate.value, 2.4e6f)) << "the rate tag that crossed the double-typed block did not reach the float member";
    };

    "a default-tag value the graph cannot convert does not stop the graph"_test = [] {
        gr::scheduler::Simple<>      scheduler;
        qa_settings::NarrowRateSink* sink = nullptr;
        {
            gr::Graph flow;
            auto&     source = flow.emplaceBlock<qa_settings::BadTagSource>();
            sink             = &flow.emplaceBlock<qa_settings::NarrowRateSink>();
            qa_settings::coupleRate(gr::settings::detail::settingsOf(*sink));
            expect(flow.connect<"out", "in">(source, *sink).has_value());
            expect(scheduler.exchange(std::move(flow)).has_value());
        }

        expect(scheduler.runAndWait().has_value()) << "a rejected tag value took the graph down";
        expect(gt(sink->nReceived, 0UZ)) << "the sink received nothing";
        expect(eq(sink->settings.sample_rate.value, 1.0f)) << "an unconvertible value reached the member";
    };
};

const boost::ut::suite<"settings hooks under the block lock"> hookLockTests = [] {
    using namespace boost::ut;
    using namespace std::chrono_literals;

    "an apply hook that stops its block runs the stop hook and copies its status under the block lock"_test = [] {
        qa_settings::HookCaller block;
        qa_settings::startAlone(block);
        block.inApply = [](qa_settings::HookCaller& self) { self.requestStop(); };

        expect(block.set({{"gain", 2.0f}}, gr::settings::noWait).has_value());
        std::ignore   = gr::testing::applyStagedForTest(gr::settings::detail::settingsOf(block));
        block.inApply = {};
        expect(eq(block.nStops.load(), 1));
        expect(block.state() == gr::lifecycle::State::REQUESTED_STOP);
        expect(eq(block.get<float>("reading").value_or(0.0f), 7.0f)) << "the stop hook's status value was not copied";
        expect(eq(block.settings.gain.value, 2.0f));
        qa_settings::stopAlone(block);
    };

    "a set during an apply hook that does not return is refused at its timeout, and the next set takes"_test = [] {
        qa_settings::WaitingHook block;
        qa_settings::startAlone(block);
        std::atomic<bool> inHook{false};
        std::atomic<bool> release{false};
        block.inApply = [&] {
            inHook.store(true);
            inHook.notify_all();
            release.wait(false);
        };

        expect(block.set({{"gain", 2.0f}}, gr::settings::noWait).has_value());
        std::thread worker([&block] { std::ignore = gr::testing::applyStagedForTest(gr::settings::detail::settingsOf(block)); });
        inHook.wait(false);
        const std::expected<gr::settings::Values, gr::settings::Rejected> timedOut = block.set({{"gain", 3.0f}}, 50ms);
        release.store(true);
        release.notify_all();
        worker.join();
        block.inApply = {};

        expect(fatal(!timedOut.has_value())) << "a set during the stuck hook was taken";
        expect(fatal(eq(timedOut.error().size(), 1UZ)));
        expect(eq(timedOut.error().front().key, std::string("gain")));
        const std::string& reason = timedOut.error().front().reason;
        expect(reason.starts_with("timed out after ") && reason.ends_with(" ms waiting for the block's settings")) << reason;
        expect(eq(block.settings.gain.value, 2.0f));
        expect(!gr::settings::detail::FrameworkAccess::hasWork(gr::settings::detail::settingsOf(block))) << "the refused set left its change pending";

        expect(block.set({{"gain", 4.0f}}, gr::settings::noWait).has_value()) << "the set after the hook returned was refused";
        std::ignore = gr::testing::applyStagedForTest(gr::settings::detail::settingsOf(block));
        expect(eq(block.settings.gain.value, 4.0f));
        qa_settings::stopAlone(block);
    };

    "a stop from another thread during the apply hook waits for the hook"_test = [] {
        qa_settings::WaitingHook block;
        qa_settings::startAlone(block);
        std::atomic<bool> inHook{false};
        std::atomic<bool> stopping{false};
        std::atomic<bool> stopped{false};
        bool              stoppedDuringHook = true;
        block.inApply                       = [&] {
            inHook.store(true);
            inHook.notify_all();
            stopping.wait(false);
            stoppedDuringHook = qa_settings::risesWithin(stopped, 100ms);
        };

        expect(block.set({{"gain", 2.0f}}, gr::settings::noWait).has_value());
        std::thread stopper([&] {
            inHook.wait(false);
            stopping.store(true);
            stopping.notify_all();
            qa_settings::stopAlone(block);
            stopped.store(true);
        });
        std::ignore = gr::testing::applyStagedForTest(gr::settings::detail::settingsOf(block));
        stopper.join();
        block.inApply = {};

        expect(!stoppedDuringHook) << "the stop ended while the apply hook ran";
        expect(eq(block.nStops.load(), 1));
        expect(block.state() == gr::lifecycle::State::STOPPED);
        expect(eq(block.settings.gain.value, 2.0f)) << "the stop took back the value the hook's batch applied";
    };
};
