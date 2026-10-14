#include <boost/ut.hpp>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#if defined(__unix__) && !defined(__EMSCRIPTEN__)
#include <csignal>
#include <sys/wait.h>
#include <unistd.h>
#endif

#include <gnuradio-4.0/Block.hpp>
#include <gnuradio-4.0/Graph.hpp>
#include <gnuradio-4.0/Scheduler.hpp>
#include <gnuradio-4.0/Sequence.hpp>
#include <gnuradio-4.0/Settings.hpp>

namespace qa_settings {

struct TunableBlock : gr::Block<TunableBlock> {
    gr::PortIn<float>  in;
    gr::PortOut<float> out;

    gr::Annotated<float, "gain">        gain        = 1.0f;
    gr::Annotated<float, "sample rate"> sample_rate = 1000.0f;

    GR_MAKE_REFLECTABLE(TunableBlock, in, out, gain, sample_rate);

    [[nodiscard]] constexpr float processOne(float value) const noexcept { return value * gain; }
};

// the common shape: the settingsChanged callback reads settings() back
struct ReentrantBlock : gr::Block<ReentrantBlock> {
    gr::PortIn<float>  in;
    gr::PortOut<float> out;

    gr::Annotated<float, "gain"> gain = 1.0f;

    GR_MAKE_REFLECTABLE(ReentrantBlock, in, out, gain);

    std::size_t _nCallbacks    = 0UZ;
    std::size_t _nKeysObserved = 0UZ;

    [[nodiscard]] constexpr float processOne(float value) const noexcept { return value * gain; }

    void settingsChanged(const gr::property_map& /*oldSettings*/, const gr::property_map& /*newSettings*/) {
        _nCallbacks++;
        _nKeysObserved += this->settings().get().size();
        std::ignore = this->settings().stagedParameters();
    }
};

struct SettingsChangedReadsDuringResetBlock : gr::Block<SettingsChangedReadsDuringResetBlock> {
    gr::Annotated<float, "gain"> gain = 1.0f;

    GR_MAKE_REFLECTABLE(SettingsChangedReadsDuringResetBlock, gain);

    bool        _resetInProgress = false;
    std::size_t _nResetCallbacks = 0UZ;
    std::size_t _nResetChanges   = 0UZ;
    float       _gainObserved    = 0.0f;

    void settingsChanged(const gr::property_map& /*oldSettings*/, const gr::property_map& /*newSettings*/) {
        std::ignore = this->settings().get();
        std::ignore = this->settings().stagedParameters();
        if (_resetInProgress) {
            _nResetChanges++;
            _gainObserved = gain.value;
            std::ignore   = this->settings().setStaged({{"gain", 3.0f}});
        }
    }

    void reset() { _nResetCallbacks++; }
};

struct ResetReadsSettingsBlock : gr::Block<ResetReadsSettingsBlock> {
    gr::Annotated<float, "gain"> gain = 1.0f;

    GR_MAKE_REFLECTABLE(ResetReadsSettingsBlock, gain);

    std::size_t _nResetCallbacks = 0UZ;
    float       _gainObserved    = 0.0f;

    void reset() {
        _nResetCallbacks++;
        std::ignore   = this->settings().get();
        std::ignore   = this->settings().stagedParameters();
        _gainObserved = gain.value;
        std::ignore   = this->settings().setStaged({{"gain", 4.0f}});
    }
};

struct ResetOrderBlock : gr::Block<ResetOrderBlock> {
    gr::Annotated<float, "gain">                     gain          = 1.0f;
    gr::Annotated<bool, "reset-default control key"> reset_default = false;

    GR_MAKE_REFLECTABLE(ResetOrderBlock, gain, reset_default);

    std::vector<std::string>            _events;
    std::vector<std::pair<float, bool>> _valuesObserved;

    void settingsChanged(const gr::property_map& /*oldSettings*/, const gr::property_map& newSettings) {
        _valuesObserved.emplace_back(gain.value, reset_default.value);
        if (newSettings.contains("gain")) {
            _events.emplace_back("settingsChanged:gain");
        }
        if (newSettings.contains(gr::tag::RESET_DEFAULTS.shortKey())) {
            _events.emplace_back("settingsChanged:reset_default");
        }
    }

    void reset() {
        _events.emplace_back("reset");
        _valuesObserved.emplace_back(gain.value, reset_default.value);
    }
};

template<typename TBlock>
void sendProperty(TBlock& block, std::string_view endpoint, gr::property_map data = {}) {
    gr::MsgPortOut toBlock;
    if (!toBlock.connect(block.msgIn).has_value()) {
        throw std::runtime_error("could not connect the settings-control message port");
    }
    gr::sendMessage<gr::message::Command::Set>(toBlock, "", endpoint, std::move(data));
    block.processScheduledMessages();
}

enum class SubprocessResult { completed, failed, timedOut, unavailable };

template<typename TScenario>
SubprocessResult runInBoundedSubprocess(TScenario&& scenario) {
#if defined(__unix__) && !defined(__EMSCRIPTEN__)
    const pid_t child = ::fork();
    if (child == 0) {
        bool succeeded = false;
        try {
            succeeded = scenario();
        } catch (...) {
        }
        ::_exit(succeeded ? 0 : 1);
    }
    if (child < 0) {
        return SubprocessResult::unavailable;
    }

    const auto terminateAndReap = [child] {
        std::ignore = ::kill(child, SIGKILL);
        int status  = 0;
        while (::waitpid(child, &status, 0) < 0 && errno == EINTR) {
        }
    };

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    int        status   = 0;
    while (std::chrono::steady_clock::now() < deadline) {
        const pid_t result = ::waitpid(child, &status, WNOHANG);
        if (result == child) {
            return WIFEXITED(status) && WEXITSTATUS(status) == 0 ? SubprocessResult::completed : SubprocessResult::failed;
        }
        if (result < 0 && errno != EINTR) {
            terminateAndReap();
            return SubprocessResult::failed;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    terminateAndReap();
    return SubprocessResult::timedOut;
#else
    // CTest also caps qa_SettingsConcurrency. Platforms without process control use that outer bound.
    return scenario() ? SubprocessResult::completed : SubprocessResult::failed;
#endif
}

template<typename TBlock>
bool setGain(TBlock& block, float gain) {
    if (!block.settings().set({{"gain", gain}}).empty() || !block.settings().activateContext().has_value()) {
        return false;
    }
    std::ignore = block.settings().applyStagedParameters();
    return block.gain.value == gain;
}

// settingsChanged blocks until a second thread has staged a value, which makes the lost update
// deterministic. The loops above reach that window only by chance.
struct BarrierBlock : gr::Block<BarrierBlock> {
    gr::PortIn<float>  in;
    gr::PortOut<float> out;

    gr::Annotated<float, "gain"> gain = 1.0f;

    GR_MAKE_REFLECTABLE(BarrierBlock, in, out, gain);

    std::atomic<bool> _inCallback{false};
    std::atomic<bool> _mayLeave{false};

    [[nodiscard]] constexpr float processOne(float value) const noexcept { return value * gain; }

    void settingsChanged(const gr::property_map& /*oldSettings*/, const gr::property_map& /*newSettings*/) {
        _inCallback.store(true);
        _inCallback.notify_all();
        _mayLeave.wait(false); // released by the second thread once it has staged
    }
};

// gain is range-limited, so an out-of-range staged value is rejected by the annotation's validator
struct ValidatingBlock : gr::Block<ValidatingBlock> {
    gr::PortIn<float>  in;
    gr::PortOut<float> out;

    gr::Annotated<float, "gain", gr::Limits<0.0f, 10.0f>> gain        = 1.0f;
    gr::Annotated<float, "sample rate">                   sample_rate = 1000.0f;

    GR_MAKE_REFLECTABLE(ValidatingBlock, in, out, gain, sample_rate);

    [[nodiscard]] constexpr float processOne(float value) const noexcept { return value * gain; }
};

// declares two default-tag keys with types other than the canonical float32 sample_rate and gr::Size_t
// num_channels, as an application using double rates does
struct WideRateBlock : gr::Block<WideRateBlock> {
    gr::PortIn<float>  in;
    gr::PortOut<float> out;

    gr::Annotated<double, "sample rate">        sample_rate  = 1.0;
    gr::Annotated<std::int64_t, "num channels"> num_channels = 1;

    GR_MAKE_REFLECTABLE(WideRateBlock, in, out, sample_rate, num_channels);

    [[nodiscard]] constexpr float processOne(float value) const noexcept { return value; }
};

struct NarrowRateSink : gr::Block<NarrowRateSink> {
    gr::PortIn<float> in;

    gr::Annotated<float, "sample rate"> sample_rate = 1.0f;

    GR_MAKE_REFLECTABLE(NarrowRateSink, in, sample_rate);

    std::size_t _nReceived = 0UZ;

    void processOne(float) { _nReceived++; }
};

struct RateSource : gr::Block<RateSource> {
    gr::PortOut<float> out;

    gr::Annotated<float, "sample rate"> sample_rate = 1.0f;

    GR_MAKE_REFLECTABLE(RateSource, out, sample_rate);

    static constexpr std::size_t kSamples = 4096UZ;

    std::size_t _nProduced = 0UZ;

    float processOne() {
        if (++_nProduced >= kSamples) {
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
            this->publishTag(gr::property_map{{"sample_rate", std::string("not-a-number")}}, 0UZ);
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

    "concurrent set/setStaged/apply/activateContext keep the maps intact"_test = [] {
        constexpr std::size_t kIterations = 100UZ;

        qa_settings::TunableBlock block;
        block.init(std::make_shared<gr::Sequence>());

        std::atomic<std::size_t> nApplied{0UZ};

        {
            std::vector<std::jthread> workers;
            workers.emplace_back([&block] {
                for (std::size_t i = 0UZ; i < kIterations; ++i) {
                    std::ignore = block.settings().set({{"gain", static_cast<float>(i)}});
                }
            });
            workers.emplace_back([&block] {
                for (std::size_t i = 0UZ; i < kIterations; ++i) {
                    std::ignore = block.settings().setStaged({{"sample_rate", static_cast<float>(1000U + i)}});
                }
            });
            workers.emplace_back([&block, &nApplied] {
                for (std::size_t i = 0UZ; i < kIterations; ++i) {
                    std::ignore = block.settings().applyStagedParameters();
                    nApplied.fetch_add(1UZ, std::memory_order_relaxed);
                }
            });
            workers.emplace_back([&block] {
                for (std::size_t i = 0UZ; i < kIterations; ++i) {
                    std::ignore = block.settings().activateContext();
                    std::ignore = block.settings().get();
                    std::ignore = block.settings().stagedParameters();
                }
            });
        }

        expect(eq(nApplied.load(), kIterations)) << "the applying thread did not finish";
        expect(gt(block.settings().get().size(), 0UZ)) << "the active parameter map is empty after the concurrent run";
        expect(ge(block.settings().getNStoredParameters(), 1U)) << "no stored parameter set survived the concurrent run";
    };

    "a settingsChanged callback may read settings() back"_test = [] {
        qa_settings::ReentrantBlock block;
        block.init(std::make_shared<gr::Sequence>());

        std::ignore = block.settings().set({{"gain", 2.0f}});
        std::ignore = block.settings().activateContext();
        std::ignore = block.settings().applyStagedParameters();

        expect(gt(block._nCallbacks, 0UZ)) << "settingsChanged was never invoked";
        expect(gt(block._nKeysObserved, 0UZ)) << "the callback could not read the settings back";
        expect(eq(block.gain.value, 2.0f)) << "the staged value was not applied";
    };

    "reset-default callbacks may read and stage settings"_test = [] {
        const auto settingsChangedResult = qa_settings::runInBoundedSubprocess([] {
            qa_settings::SettingsChangedReadsDuringResetBlock block;
            block.init(std::make_shared<gr::Sequence>());
            if (!qa_settings::setGain(block, 2.0f)) {
                return false;
            }

            block._resetInProgress = true;
            qa_settings::sendProperty(block, gr::block::property::kResetDefaults);
            block._resetInProgress = false;

            if (block.gain.value != 1.0f || block._gainObserved != 1.0f || block._nResetChanges != 1UZ || block._nResetCallbacks != 1UZ || !block.settings().stagedParameters().contains("gain")) {
                return false;
            }
            std::ignore = block.settings().applyStagedParameters();
            return block.gain.value == 3.0f;
        });
        expect(eq(static_cast<int>(settingsChangedResult), static_cast<int>(qa_settings::SubprocessResult::completed))) << "settingsChanged could not read and stage settings during reset-default processing";

        const auto resetResult = qa_settings::runInBoundedSubprocess([] {
            qa_settings::ResetReadsSettingsBlock block;
            block.init(std::make_shared<gr::Sequence>());
            if (!qa_settings::setGain(block, 2.0f)) {
                return false;
            }

            qa_settings::sendProperty(block, gr::block::property::kResetDefaults);
            if (block.gain.value != 1.0f || block._gainObserved != 1.0f || block._nResetCallbacks != 1UZ || !block.settings().stagedParameters().contains("gain")) {
                return false;
            }
            std::ignore = block.settings().applyStagedParameters();
            return block.gain.value == 4.0f;
        });
        expect(eq(static_cast<int>(resetResult), static_cast<int>(qa_settings::SubprocessResult::completed))) << "reset could not read and stage settings during reset-default processing";
    };

    "the ResetDefaults property invokes settingsChanged before one reset"_test = [] {
        qa_settings::ResetOrderBlock block;
        block.init(std::make_shared<gr::Sequence>());
        expect(qa_settings::setGain(block, 2.0f));
        block._events.clear();
        block._valuesObserved.clear();

        qa_settings::sendProperty(block, gr::block::property::kResetDefaults);

        expect(eq(block.gain.value, 1.0f)) << "the ResetDefaults property did not restore the default gain";
        expect(eq(block._events.size(), 2UZ)) << "the ResetDefaults property produced the wrong number of callbacks";
        if (block._events.size() == 2UZ) {
            expect(eq(block._events[0], std::string("settingsChanged:gain"))) << "settingsChanged did not precede reset";
            expect(eq(block._events[1], std::string("reset"))) << "the reset callback did not finish the operation";
        }
        expect(eq(block._valuesObserved.size(), 2UZ)) << "the callbacks did not both observe the restored settings";
        if (block._valuesObserved.size() == 2UZ) {
            expect(eq(block._valuesObserved[0].first, 1.0f)) << "settingsChanged observed gain before defaults were restored";
            expect(!block._valuesObserved[0].second) << "settingsChanged observed reset_default before defaults were restored";
            expect(eq(block._valuesObserved[1].first, 1.0f)) << "reset observed gain before defaults were restored";
            expect(!block._valuesObserved[1].second) << "reset observed reset_default before defaults were restored";
        }

        block._events.clear();
        block._valuesObserved.clear();
        qa_settings::sendProperty(block, gr::block::property::kResetDefaults);
        expect(eq(block._events.size(), 1UZ)) << "an unchanged reset should not produce settingsChanged";
        if (block._events.size() == 1UZ) {
            expect(eq(block._events[0], std::string("reset"))) << "an unchanged reset should still invoke reset once";
        }
    };

    "the ResetDefaults property control is not an accepted staged setting"_test = [] {
        qa_settings::ResetOrderBlock block;
        block.init(std::make_shared<gr::Sequence>());
        block._events.clear();
        block._valuesObserved.clear();

        const auto fullKey = static_cast<std::pmr::string>(gr::tag::RESET_DEFAULTS);
        const auto notSet  = block.settings().setStaged({{fullKey, true}});
        expect(notSet.contains(fullKey)) << "the qualified reset-default control unexpectedly entered the staged settings batch";
        expect(!block.settings().stagedParameters().contains(fullKey)) << "the rejected reset-default control was staged";

        const auto shortNameNotSet = block.settings().setStaged({{gr::tag::RESET_DEFAULTS.shortKey(), true}});
        expect(shortNameNotSet.empty()) << "the reflected short-name setting was rejected";
        std::ignore = block.settings().applyStagedParameters();
        expect(eq(block._events.size(), 1UZ)) << "the ordinary short-name setting produced an unexpected callback count";
        if (block._events.size() == 1UZ) {
            expect(eq(block._events[0], std::string("settingsChanged:reset_default"))) << "the ordinary short-name setting was reinterpreted as a reset control";
        }
    };

    "a value staged inside the settingsChanged callback survives the apply that is running"_test = [] {
        qa_settings::BarrierBlock block;
        block.init(std::make_shared<gr::Sequence>());

        std::ignore = block.settings().set({{"gain", 2.0f}});
        std::ignore = block.settings().activateContext();

        std::atomic<bool> staged{false};
        std::thread       stager([&block, &staged] {
            block._inCallback.wait(false);                              // the apply has released the lock for the callback
            std::ignore = block.settings().setStaged({{"gain", 3.0f}}); // which is what lets this succeed
            staged.store(true);
            block._mayLeave.store(true);
            block._mayLeave.notify_all();
        });

        std::ignore = block.settings().applyStagedParameters();
        stager.join();

        expect(staged.load()) << "the second thread never reached setStaged inside the callback window";
        expect(eq(block.gain.value, 2.0f)) << "the first batch was not applied";
        expect(block.settings().stagedParameters().contains(std::pmr::string("gain"))) << "the value staged inside the callback was erased by the apply that was running";

        std::ignore = block.settings().applyStagedParameters();
        expect(eq(block.gain.value, 3.0f)) << "the surviving staged value was never applied";
    };

    // the per-work() read of the auto-forward set is unlocked because the set is fixed for the run
    "the auto-forward set cannot be added to while the block is running"_test = [] {
        qa_settings::TunableBlock block;
        block.init(std::make_shared<gr::Sequence>());

        block.settings().addAutoForwardParameters({"before_start"});
        expect(block.settings().autoForwardParameters().contains("before_start")) << "a key added before the run was refused";

        expect(block.changeStateTo(gr::lifecycle::State::INITIALISED).has_value());
        expect(block.changeStateTo(gr::lifecycle::State::RUNNING).has_value());

        block.settings().addAutoForwardParameters({"after_start"});
        expect(not block.settings().autoForwardParameters().contains("after_start")) << "a key added on a running block reached the set the work path reads unlocked";

        expect(block.changeStateTo(gr::lifecycle::State::REQUESTED_STOP).has_value());
        expect(block.changeStateTo(gr::lifecycle::State::STOPPED).has_value());

        block.settings().addAutoForwardParameters({"after_stop"});
        expect(block.settings().autoForwardParameters().contains("after_stop")) << "the run is over, so the set is writable again";
    };

    "a rejected value is neither applied nor forwarded"_test = [] {
        qa_settings::ValidatingBlock block;
        block.init(std::make_shared<gr::Sequence>());

        std::ignore                                  = block.settings().set({{"gain", 99.0f}, {"sample_rate", 48000.0f}});
        std::ignore                                  = block.settings().activateContext();
        const gr::ApplyStagedParametersResult result = block.settings().applyStagedParameters();

        expect(result.failedParameters.contains("gain")) << "the out-of-range value was not reported as rejected";
        expect(!result.appliedParameters.contains("gain")) << "the out-of-range value was reported as applied";
        expect(!result.forwardParameters.contains("gain")) << "the out-of-range value was forwarded downstream";
        expect(eq(block.gain.value, 1.0f)) << "the out-of-range value reached the block";

        expect(!result.failedParameters.contains("sample_rate")) << "a valid value was reported as rejected";
        expect(result.forwardParameters.contains("sample_rate")) << "a valid auto-forward value was not forwarded";
        expect(eq(block.sample_rate.value, 48000.0f)) << "a valid value was not applied";
    };

    "a default-tag value of another numeric type is converted to the member's type"_test = [] {
        qa_settings::WideRateBlock block;
        block.init(std::make_shared<gr::Sequence>());

        block.settings().autoUpdate(gr::Tag{0UZ, {{"sample_rate", 2.4e6f}, {"num_channels", 4U}}});
        const gr::ApplyStagedParametersResult result = block.settings().applyStagedParameters();

        expect(result.failedParameters.empty()) << "a convertible numeric value was reported as rejected";
        expect(eq(block.sample_rate.value, 2.4e6)) << "the float32 tag did not reach the double member";
        expect(eq(block.num_channels.value, std::int64_t(4))) << "the gr::Size_t tag did not reach the int64 member";
        expect(result.forwardParameters.contains("sample_rate")) << "the converted value was not forwarded downstream";
    };

    "a wider default-tag value is narrowed onto the canonical member type"_test = [] {
        qa_settings::NarrowRateSink block;
        block.init(std::make_shared<gr::Sequence>());

        block.settings().autoUpdate(gr::Tag{0UZ, {{"sample_rate", 2.4e6}}});
        std::ignore = block.settings().applyStagedParameters();

        expect(eq(block.sample_rate.value, 2.4e6f)) << "the float64 tag did not reach the float member";
    };

    "a default-tag value outside the member's range is not applied"_test = [] {
        qa_settings::NarrowRateSink block;
        block.init(std::make_shared<gr::Sequence>());

        block.settings().autoUpdate(gr::Tag{0UZ, {{"sample_rate", 1e300}}});
        std::ignore = block.settings().applyStagedParameters();

        expect(eq(block.sample_rate.value, 1.0f)) << "an out-of-range value was applied instead of range-checked";
    };

    "a non-numeric default-tag value is not applied"_test = [] {
        qa_settings::WideRateBlock block;
        block.init(std::make_shared<gr::Sequence>());

        block.settings().autoUpdate(gr::Tag{0UZ, {{"sample_rate", std::string("not-a-number")}}});
        std::ignore = block.settings().applyStagedParameters();

        expect(eq(block.sample_rate.value, 1.0)) << "a string was coerced into a numeric member";
    };

    "a float32 rate tag crosses a double-typed block and reaches a float sink"_test = [] {
        gr::scheduler::Simple        scheduler;
        qa_settings::WideRateBlock*  wideBlock = nullptr;
        qa_settings::NarrowRateSink* sink      = nullptr;
        {
            gr::Graph flow;
            auto&     source = flow.emplaceBlock<qa_settings::RateSource>({{"sample_rate", 2.4e6f}});
            wideBlock        = &flow.emplaceBlock<qa_settings::WideRateBlock>();
            sink             = &flow.emplaceBlock<qa_settings::NarrowRateSink>();
            expect(flow.connect<"out", "in">(source, *wideBlock).has_value());
            expect(flow.connect<"out", "in">(*wideBlock, *sink).has_value());
            expect(scheduler.exchange(std::move(flow)).has_value());
        }

        expect(scheduler.runAndWait().has_value()) << "the graph did not run to completion";
        expect(gt(sink->_nReceived, 0UZ)) << "the sink received nothing";
        expect(eq(wideBlock->sample_rate.value, 2.4e6)) << "the forwarded float32 rate did not reach the double member";
        expect(eq(sink->sample_rate.value, 2.4e6f)) << "the re-forwarded float64 rate did not reach the float member";
    };

    "a default-tag value the graph cannot convert does not stop the graph"_test = [] {
        gr::scheduler::Simple        scheduler;
        qa_settings::NarrowRateSink* sink = nullptr;
        {
            gr::Graph flow;
            auto&     source = flow.emplaceBlock<qa_settings::BadTagSource>();
            sink             = &flow.emplaceBlock<qa_settings::NarrowRateSink>();
            expect(flow.connect<"out", "in">(source, *sink).has_value());
            expect(scheduler.exchange(std::move(flow)).has_value());
        }

        expect(scheduler.runAndWait().has_value()) << "a rejected tag value took the graph down";
        expect(gt(sink->_nReceived, 0UZ)) << "the sink received nothing";
        expect(eq(sink->sample_rate.value, 1.0f)) << "an unconvertible value reached the member";
    };
};

int main() { /* tests are statically registered */ }
