#include <boost/ut.hpp>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <expected>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <type_traits>
#include <utility>

#include <gnuradio-4.0/Block.hpp>
#include <gnuradio-4.0/BlockMerging.hpp>
#include <gnuradio-4.0/Graph.hpp>
#include <gnuradio-4.0/Scheduler.hpp>
#include <gnuradio-4.0/meta/simd.hpp>

#include "settings_utils.hpp"

/**
 * @brief A merged block's processOne resolves for both a scalar and a SIMD argument pack.
 *
 * The merged return type is `meta::simdize<ReturnType, detail::simdWidth<Ts...>()>`, so naming it is what instantiates
 * the width helper. Nothing else in this repository instantiates the helper's SIMD branch: the example in
 * `core/src/main.cpp` merges scalar blocks, and that file is not built. A fault confined to that branch therefore
 * reaches no compiler until a downstream project merges SIMD-capable blocks.
 *
 * The checks are compile-time only. The blocks are defined here because gnuradio4-core carries no standard block
 * library, so a core test may not depend on one.
 */

namespace qa_block_merging {

using namespace gr;

template<typename T, T kScale, typename R = decltype(std::declval<T>() * std::declval<T>())>
struct Scale : Block<Scale<T, kScale, R>> {
    PortIn<T>  original;
    PortOut<R> scaled;

    GR_MAKE_REFLECTABLE(Scale, original, scaled);

    template<meta::t_or_simd<T> V>
    [[nodiscard]] constexpr auto processOne(V value) const noexcept {
        return value * kScale;
    }
};

template<typename T, typename R = decltype(std::declval<T>() + std::declval<T>())>
struct Adder : Block<Adder<T>> {
    PortIn<T>  addend0;
    PortIn<T>  addend1;
    PortOut<R> sum;

    GR_MAKE_REFLECTABLE(Adder, addend0, addend1, sum);

    template<meta::t_or_simd<T> V>
    [[nodiscard]] constexpr auto processOne(V lhs, V rhs) const noexcept {
        return lhs + rhs;
    }
};

using Merged = decltype(MergeByIndex<Scale<int, -1>, 0, MergeByIndex<Scale<int, 2>, 0, Adder<int>, 0>, 0>());
using Simd   = meta::simdize<int>;

using SimdResult = decltype(std::declval<const Merged&>().processOne(std::declval<Simd>(), std::declval<Simd>()));
static_assert(meta::any_simd<SimdResult>, "a SIMD argument pack keeps the result a SIMD vector");
static_assert(SimdResult::size() == Simd::size(), "the merged width is the width the inputs share");

using ScalarResult = decltype(std::declval<const Merged&>().processOne(0, 0));
static_assert(std::is_same_v<ScalarResult, int>, "a scalar argument pack keeps the result scalar");

/// scales its input; its settings hook fails a name `kTakenName`
struct NamedPass : Block<NamedPass> {
    PortIn<float>  in;
    PortOut<float> out;

    struct Settings {
        float scale = 1.f;

        GR_MAKE_REFLECTABLE(Settings, scale);
    } settings;

    GR_MAKE_REFLECTABLE(NamedPass, in, out, settings);

    static constexpr std::string_view kTakenName = "taken";

    void applySettings([[maybe_unused]] const Settings& s) {
        if (this->name.value == kTakenName) {
            throw std::runtime_error("the name is taken");
        }
    }

    [[nodiscard]] constexpr float processOne(float value) const noexcept { return value * settings.scale; }
};

struct EndlessSource : Block<EndlessSource> {
    PortOut<float> out;

    GR_MAKE_REFLECTABLE(EndlessSource, out);

    [[nodiscard]] constexpr float processOne() const noexcept { return 1.f; }
};

inline std::atomic<std::size_t> gReceived{0UZ}; // the samples CountingSink took, read from the test's thread

struct CountingSink : Block<CountingSink> {
    PortIn<float> in;

    GR_MAKE_REFLECTABLE(CountingSink, in);

    void processOne(float /*value*/) { gReceived.fetch_add(1UZ, std::memory_order_relaxed); }
};

/// scales its input by `gain`, and keeps the gain its last call used
struct Tuned : Block<Tuned> {
    PortIn<float>  in;
    PortOut<float> out;

    struct Settings {
        float gain = 1.f;

        GR_MAKE_REFLECTABLE(Settings, gain);
    } settings;

    GR_MAKE_REFLECTABLE(Tuned, in, out, settings);

    mutable std::atomic<float> gainUsed{0.f};

    [[nodiscard]] float processOne(float value) const noexcept {
        gainUsed.store(settings.gain, std::memory_order_relaxed);
        return value * settings.gain;
    }
};

/// scales its input by `gain`; its first call sets its own gain and reports the gain it ran in its status
struct SelfTuned : Block<SelfTuned> {
    PortIn<float>  in;
    PortOut<float> out;

    struct Settings {
        float gain = 1.f;

        GR_MAKE_REFLECTABLE(Settings, gain);
    } settings;

    struct Status {
        float gain_run = 0.f;

        GR_MAKE_REFLECTABLE(Status, gain_run);
    } status;

    GR_MAKE_REFLECTABLE(SelfTuned, in, out, settings, status);

    std::optional<std::expected<settings::Values, settings::Rejected>> ownSet{};
    std::atomic<bool>                                                  called{false}; // the test reads the results once this is set

    [[nodiscard]] float processOne(float value) {
        if (!called.load(std::memory_order_relaxed)) {
            ownSet          = this->set(gr::property_map{{"gain", 2.f}}, settings::noWait);
            status.gain_run = 3.f;
            called.store(true, std::memory_order_release);
        }
        return value * settings.gain;
    }
};

/// declares a gain and passes it to the Tuned it holds; its work call applies the member's pending settings
struct ExposedTuned : Block<ExposedTuned> {
    PortIn<float>  in;
    PortOut<float> out;

    struct Settings {
        float gain = 1.f;

        GR_MAKE_REFLECTABLE(Settings, gain);
    } settings;

    GR_MAKE_REFLECTABLE(ExposedTuned, in, out, settings);

    Tuned member;

    // a gain the member refuses takes back its batch
    void applySettings(const Settings& s, const Settings& old) {
        if (s.gain == old.gain) {
            return;
        }
        if (std::expected<gr::settings::Values, gr::settings::Rejected> staged = member.set(gr::property_map{{"gain", s.gain}}, gr::settings::noWait); !staged) {
            this->fail(s.gain, gr::settings::describeRejected(staged.error()));
            return;
        }
        std::ignore = gr::testing::applyStagedForTest(gr::settings::detail::settingsOf(member));
    }

    void applyMemberSettings() { detail::applyPendingMemberSettings(member); }

    [[nodiscard]] float processOne(float value) { return member.processOne(value); }
};

} // namespace qa_block_merging

const boost::ut::suite<"a merged block's settings"> mergedSettingsTests = [] {
    using namespace boost::ut;
    using namespace qa_block_merging;

    "a running merged block applies a set key on its members within its own work call"_test = [] {
        using PassTwice = Merge<NamedPass, "out", NamedPass, "in">;
        gReceived.store(0UZ);
        scheduler::Simple<> sched;
        PassTwice*          merged = nullptr;
        {
            Graph flow;
            auto& source = flow.emplaceBlock<EndlessSource>();
            merged       = &flow.emplaceBlock<PassTwice>();
            auto& sink   = flow.emplaceBlock<CountingSink>();
            expect(fatal(flow.connect<"out", "in">(source, *merged).has_value()));
            expect(fatal(flow.connect<"out", "in">(*merged, sink).has_value()));
            expect(fatal(sched.exchange(std::move(flow)).has_value()));
        }
        std::thread runner([&sched] { std::ignore = sched.runAndWait(); });
        const auto  startedBy = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (gReceived.load(std::memory_order_relaxed) == 0UZ && std::chrono::steady_clock::now() < startedBy) {
            std::this_thread::yield();
        }
        expect(gt(gReceived.load(std::memory_order_relaxed), 0UZ)) << "the graph does not run";

        // the call waits the default timeout for the merged block's apply; a member that waited for a work call of
        // its own would hold that apply past it, and the key would come back pending
        const std::expected<settings::Values, settings::Rejected> renamed = merged->set({{"name", std::string("renamed")}});
        expect(renamed.has_value() && renamed->empty()) << "the merged block's apply outlasts the call's timeout: a member waits";
        const std::expected<settings::Values, settings::Rejected> taken = merged->set({{"name", std::string(NamedPass::kTakenName)}});
        expect(taken.has_value() && taken->size() == 1UZ && taken->contains("name") && taken->at("name").isFailed() && taken->at("name").reason.contains("the name is taken")) << "a member's failed key does not fail the merged block's key";

        std::ignore = sched.changeStateTo(lifecycle::State::REQUESTED_STOP);
        runner.join();
        expect(eq(merged->_leftBlock.name.value, std::string("renamed")) && eq(merged->_rightBlock.name.value, std::string("renamed"))) << "a member's field does not take the value";
        expect(eq(merged->name.value, std::string("renamed"))) << "the merged block keeps a name a member failed";
    };

    "a value set on a member from another thread reaches the member's field at the merged block's work call"_test = [] {
        using TunedPass = Merge<Tuned, "out", NamedPass, "in">;
        gReceived.store(0UZ);
        scheduler::Simple<> sched;
        TunedPass*          merged = nullptr;
        {
            Graph flow;
            auto& source = flow.emplaceBlock<EndlessSource>();
            merged       = &flow.emplaceBlock<TunedPass>();
            auto& sink   = flow.emplaceBlock<CountingSink>();
            expect(fatal(flow.connect<"out", "in">(source, *merged).has_value()));
            expect(fatal(flow.connect<"out", "in">(*merged, sink).has_value()));
            expect(fatal(sched.exchange(std::move(flow)).has_value()));
        }
        std::thread runner([&sched] { std::ignore = sched.runAndWait(); });
        const auto  deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (gReceived.load(std::memory_order_relaxed) == 0UZ && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::yield();
        }
        expect(gt(gReceived.load(std::memory_order_relaxed), 0UZ)) << "the graph does not run";

        std::thread device([merged] { std::ignore = merged->_leftBlock.set(gr::property_map{{"gain", 2.5f}}, settings::noWait); });
        device.join();
        while (merged->_leftBlock.gainUsed.load(std::memory_order_relaxed) != 2.5f && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::yield();
        }
        std::ignore = sched.changeStateTo(lifecycle::State::REQUESTED_STOP);
        runner.join();
        expect(eq(merged->_leftBlock.settings.gain, 2.5f)) << "the member's field did not take the value set";
        expect(eq(merged->_leftBlock.get<float>("gain").value_or(0.f), 2.5f));
    };

    "a member's set on its own settings is refused, and its status reaches a host at the merged block's work call"_test = [] {
        using SelfTunedPass = Merge<SelfTuned, "out", NamedPass, "in">;
        gReceived.store(0UZ);
        scheduler::Simple<> sched;
        SelfTunedPass*      merged = nullptr;
        {
            Graph flow;
            auto& source = flow.emplaceBlock<EndlessSource>();
            merged       = &flow.emplaceBlock<SelfTunedPass>();
            auto& sink   = flow.emplaceBlock<CountingSink>();
            expect(fatal(flow.connect<"out", "in">(source, *merged).has_value()));
            expect(fatal(flow.connect<"out", "in">(*merged, sink).has_value()));
            expect(fatal(sched.exchange(std::move(flow)).has_value()));
        }
        std::thread runner([&sched] { std::ignore = sched.runAndWait(); });
        const auto  deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (!merged->_leftBlock.called.load(std::memory_order_acquire) && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::yield();
        }
        // a host's read asks for the status, which the merged block's next work call copies for each member
        while (merged->_leftBlock.get<float>("gain_run").value_or(0.f) != 3.f && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::yield();
        }
        const float gainRun = merged->_leftBlock.get<float>("gain_run").value_or(0.f);
        std::ignore         = sched.changeStateTo(lifecycle::State::REQUESTED_STOP);
        runner.join();
        const SelfTuned& member = merged->_leftBlock;
        expect(fatal(member.called.load(std::memory_order_acquire))) << "the member was never called";
        expect(fatal(member.ownSet.has_value()));
        expect(fatal(!member.ownSet->has_value())) << "the member's own set was taken";
        expect(fatal(!member.ownSet->error().empty()));
        expect(member.ownSet->error().front().reason.contains("own thread")) << member.ownSet->error().front().reason;
        expect(eq(gainRun, 3.f)) << "the member's status did not reach a host";
    };

    "a value set on a member reaches the member and not the block that declares the same key"_test = [] {
        gReceived.store(0UZ);
        scheduler::Simple<> sched;
        ExposedTuned*       exposed = nullptr;
        {
            Graph flow;
            auto& source = flow.emplaceBlock<EndlessSource>();
            exposed      = &flow.emplaceBlock<ExposedTuned>();
            auto& sink   = flow.emplaceBlock<CountingSink>();
            expect(fatal(flow.connect<"out", "in">(source, *exposed).has_value()));
            expect(fatal(flow.connect<"out", "in">(*exposed, sink).has_value()));
            expect(fatal(sched.exchange(std::move(flow)).has_value()));
        }
        std::thread runner([&sched] { std::ignore = sched.runAndWait(); });
        const auto  deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (gReceived.load(std::memory_order_relaxed) == 0UZ && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::yield();
        }
        expect(gt(gReceived.load(std::memory_order_relaxed), 0UZ)) << "the graph does not run";

        std::thread device([exposed] { std::ignore = exposed->member.set(gr::property_map{{"gain", 4.f}}, gr::settings::noWait); });
        device.join();
        while (exposed->member.gainUsed.load(std::memory_order_relaxed) != 4.f && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::yield();
        }
        std::ignore = sched.changeStateTo(lifecycle::State::REQUESTED_STOP);
        runner.join();
        expect(eq(exposed->member.settings.gain, 4.f)) << "the member's field did not take the value set";
        expect(eq(exposed->get<float>("gain").value_or(0.f), 1.f)) << "a value set on a member reached the block that holds it";
    };
};
