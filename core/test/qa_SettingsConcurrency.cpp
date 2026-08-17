#include <boost/ut.hpp>

#include <atomic>
#include <memory>
#include <thread>
#include <vector>

#include <gnuradio-4.0/Block.hpp>
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

// gain is range-limited, so an out-of-range staged value is rejected by the annotation's validator
struct ValidatingBlock : gr::Block<ValidatingBlock> {
    gr::PortIn<float>  in;
    gr::PortOut<float> out;

    gr::Annotated<float, "gain", gr::Limits<0.0f, 10.0f>> gain        = 1.0f;
    gr::Annotated<float, "sample rate">                   sample_rate = 1000.0f;

    GR_MAKE_REFLECTABLE(ValidatingBlock, in, out, gain, sample_rate);

    [[nodiscard]] constexpr float processOne(float value) const noexcept { return value * gain; }
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
};

int main() { /* tests are statically registered */ }
