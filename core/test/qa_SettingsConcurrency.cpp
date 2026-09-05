#include <boost/ut.hpp>

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <vector>

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
