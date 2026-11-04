#include <boost/ut.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <tuple>

#include <gnuradio-4.0/Graph.hpp>
#include <gnuradio-4.0/Scheduler.hpp>
#include <gnuradio-4.0/meta/simd.hpp>

namespace gr::qa_processone_simd {

struct Int16Source : gr::Block<Int16Source> {
    gr::PortOut<std::int16_t> out;

    GR_MAKE_REFLECTABLE(Int16Source, out);

    std::size_t _nTotal  = 0UZ;
    std::size_t _emitted = 0UZ;

    gr::work::Status processBulk(gr::OutputSpanLike auto& outSpan) {
        const std::size_t n = std::min(outSpan.size(), _nTotal - _emitted);
        std::fill_n(outSpan.begin(), n, std::int16_t{1});
        _emitted += n;
        outSpan.publish(n);
        return _emitted == _nTotal ? gr::work::Status::DONE : gr::work::Status::OK;
    }
};

struct LaneProbe : gr::Block<LaneProbe> {
    gr::PortIn<std::int16_t>  in;
    gr::PortOut<std::int16_t> out;

    GR_MAKE_REFLECTABLE(LaneProbe, in, out);

    mutable std::size_t _widestCall = 0UZ;

    template<gr::meta::t_or_simd<std::int16_t> V>
    [[nodiscard]] V processOne(V x) const noexcept {
        if constexpr (gr::meta::any_simd<V>) {
            _widestCall = std::max(_widestCall, static_cast<std::size_t>(V::size()));
        }
        return x;
    }
};

struct Int16Sink : gr::Block<Int16Sink> {
    gr::PortIn<std::int16_t> in;

    GR_MAKE_REFLECTABLE(Int16Sink, in);

    std::size_t _nReceived = 0UZ;

    gr::work::Status processBulk(gr::InputSpanLike auto& inSpan) {
        _nReceived += inSpan.size();
        std::ignore = inSpan.consume(inSpan.size());
        return gr::work::Status::OK;
    }
};

static_assert(gr::traits::block::can_processOne_simd<LaneProbe>);

} // namespace gr::qa_processone_simd

const boost::ut::suite<"processOne SIMD width"> processOneSimdWidthTests = [] {
    using namespace boost::ut;
    using namespace gr::qa_processone_simd;

    "a const processOne receives four times the native lanes, capped"_test = [] {
#if defined(__clang__) && defined(VIR_HAVE_VIR_SIMD)
        constexpr std::size_t kMaxLanes = 16UZ; // Block.hpp's cap for vir-simd's own SIMD types under Clang
#else
        constexpr std::size_t kMaxLanes = vir::stdx::simd_abi::max_fixed_size<double>;
#endif
        constexpr std::size_t kExpectedLanes = std::min(kMaxLanes, gr::meta::simdize<std::int16_t>::size() * 4UZ);
        constexpr std::size_t kSamples       = 64UZ * kExpectedLanes;

        gr::Graph flow;
        auto&     source = flow.emplaceBlock<Int16Source>(gr::property_map{{"name", std::string("src")}});
        auto&     probe  = flow.emplaceBlock<LaneProbe>(gr::property_map{{"name", std::string("probe")}});
        auto&     sink   = flow.emplaceBlock<Int16Sink>(gr::property_map{{"name", std::string("snk")}});
        source._nTotal   = kSamples;
        expect(flow.connect<"out", "in">(source, probe).has_value());
        expect(flow.connect<"out", "in">(probe, sink).has_value());

        gr::scheduler::Simple<gr::scheduler::ExecutionPolicy::singleThreaded> scheduler;
        expect(scheduler.exchange(std::move(flow)).has_value());
        expect(scheduler.runAndWait().has_value());

        expect(eq(sink._nReceived, kSamples));
        expect(eq(probe._widestCall, kExpectedLanes));
    };
};

int main() { /* tests are statically executed */ }
