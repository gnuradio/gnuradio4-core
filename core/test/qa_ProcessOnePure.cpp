#include <boost/ut.hpp>

#include <complex>
#include <cstddef>
#include <cstdint>
#include <ranges>
#include <span>
#include <tuple>
#include <vector>

#include <gnuradio-4.0/Block.hpp>
#include <gnuradio-4.0/Port.hpp>

namespace gr::qa_processone_pure {

struct Pair {
    float re;
    float im;
    bool  operator==(const Pair&) const = default;
};

// processOne is const and not SIMD-capable, so work() calls invokeProcessOnePure.
struct ComplexAffine : gr::Block<ComplexAffine> {
    gr::PortIn<std::complex<float>>  in;
    gr::PortOut<std::complex<float>> out;

    GR_MAKE_REFLECTABLE(ComplexAffine, in, out);

    [[nodiscard]] std::complex<float> processOne(std::complex<float> x) const noexcept { return {x.real() * 0.5f - x.imag() * 0.25f + 1.f, x.real() * 0.25f + x.imag() * 0.5f - 2.f}; }
};

struct PairAffine : gr::Block<PairAffine> {
    gr::PortIn<Pair>  in;
    gr::PortOut<Pair> out;

    GR_MAKE_REFLECTABLE(PairAffine, in, out);

    [[nodiscard]] Pair processOne(const Pair& x) const noexcept { return {x.re * 0.5f - x.im * 0.25f + 1.f, x.re * 0.25f + x.im * 0.5f - 2.f}; }
};

struct SumDiff : gr::Block<SumDiff> {
    gr::PortIn<std::complex<float>>  in0;
    gr::PortIn<std::complex<float>>  in1;
    gr::PortOut<std::complex<float>> sum;
    gr::PortOut<std::complex<float>> diff;

    GR_MAKE_REFLECTABLE(SumDiff, in0, in1, sum, diff);

    [[nodiscard]] std::tuple<std::complex<float>, std::complex<float>> processOne(std::complex<float> a, std::complex<float> b) const noexcept { return {a + b, a - b}; }
};

// processOne is not const, so work() calls invokeProcessOneNonConst.
struct RunningSum : gr::Block<RunningSum> {
    gr::PortIn<std::complex<float>>  in;
    gr::PortOut<std::complex<float>> out;

    GR_MAKE_REFLECTABLE(RunningSum, in, out);

    std::complex<float> _sum{};

    [[nodiscard]] std::complex<float> processOne(std::complex<float> x) noexcept {
        _sum += x;
        return _sum;
    }
};

static_assert(!gr::traits::block::can_processOne_simd<ComplexAffine>);
static_assert(!gr::traits::block::can_processOne_simd<PairAffine>);
static_assert(!gr::traits::block::can_processOne_simd<SumDiff>);

template<typename T>
T sampleAt(std::size_t i) {
    const float a = static_cast<float>(static_cast<int>((i * 37UZ) % 101UZ) - 50) * 0.125f;
    const float b = static_cast<float>(static_cast<int>((i * 59UZ) % 103UZ) - 51) * 0.0625f;
    if constexpr (std::is_same_v<T, Pair>) {
        return Pair{a, b};
    } else {
        return T(a, b);
    }
}

constexpr std::size_t kRingSize = 1024UZ;

// Counts the chunks whose samples cross the end of the ring, so the test can show that the offset into the ring is non-zero and the wrap is reached.
struct RingPosition {
    std::size_t capacity = 0UZ;
    std::size_t written  = 0UZ;
    std::size_t wrapped  = 0UZ;

    void advance(std::size_t n) {
        if (written % capacity + n > capacity) {
            ++wrapped;
        }
        written += n;
    }
};

// One block with real ring-buffer ports; every chunk is one workInternal call that starts where the previous one ended.
template<typename TBlock, typename T>
struct OneInOneOut {
    TBlock         block;
    gr::PortOut<T> source;
    gr::PortIn<T>  sink;
    RingPosition   position;
    std::size_t    nextSample = 0UZ;

    OneInOneOut() {
        std::ignore       = source.resizeBuffer(kRingSize);
        std::ignore       = source.connect(block.in);
        std::ignore       = block.out.resizeBuffer(kRingSize);
        std::ignore       = block.out.connect(sink);
        std::ignore       = block.changeStateTo(gr::lifecycle::State::INITIALISED);
        std::ignore       = block.changeStateTo(gr::lifecycle::State::RUNNING);
        position.capacity = block.out.bufferSize();
    }

    // Runs one workInternal call of n samples through ring-buffer ports and returns the output; want receives the expected samples.
    template<typename TExpected>
    std::vector<T> runChunk(std::size_t n, TExpected expected, std::vector<T>& want) {
        {
            auto span = source.template reserve<gr::SpanReleasePolicy::ProcessAll>(n);
            for (std::size_t i = 0UZ; i < n; ++i) {
                span[i] = sampleAt<T>(nextSample + i);
            }
        }
        std::ignore = block.workInternal(n);

        want.clear();
        for (std::size_t i = 0UZ; i < n; ++i) {
            want.push_back(expected(sampleAt<T>(nextSample + i)));
        }
        nextSample += n;
        position.advance(n);
        auto result = sink.template get<gr::SpanReleasePolicy::ProcessAll>(n);
        return std::vector<T>(result.begin(), result.end());
    }
};

struct TwoInTwoOut {
    SumDiff                          block;
    gr::PortOut<std::complex<float>> source0;
    gr::PortOut<std::complex<float>> source1;
    gr::PortIn<std::complex<float>>  sink0;
    gr::PortIn<std::complex<float>>  sink1;
    RingPosition                     position;
    std::size_t                      nextSample = 0UZ;

    TwoInTwoOut() {
        std::ignore       = source0.resizeBuffer(kRingSize);
        std::ignore       = source1.resizeBuffer(kRingSize);
        std::ignore       = source0.connect(block.in0);
        std::ignore       = source1.connect(block.in1);
        std::ignore       = block.sum.resizeBuffer(kRingSize);
        std::ignore       = block.diff.resizeBuffer(kRingSize);
        std::ignore       = block.sum.connect(sink0);
        std::ignore       = block.diff.connect(sink1);
        std::ignore       = block.changeStateTo(gr::lifecycle::State::INITIALISED);
        std::ignore       = block.changeStateTo(gr::lifecycle::State::RUNNING);
        position.capacity = block.sum.bufferSize();
    }

    // checks both outputs of one chunk of n samples against a + b and a - b
    bool runChunk(std::size_t n) {
        {
            auto span0 = source0.reserve<gr::SpanReleasePolicy::ProcessAll>(n);
            auto span1 = source1.reserve<gr::SpanReleasePolicy::ProcessAll>(n);
            for (std::size_t i = 0UZ; i < n; ++i) {
                span0[i] = sampleAt<std::complex<float>>(nextSample + i);
                span1[i] = sampleAt<std::complex<float>>(nextSample + i + 7UZ);
            }
        }
        std::ignore = block.workInternal(n);

        auto sumSpan  = sink0.get<gr::SpanReleasePolicy::ProcessAll>(n);
        auto diffSpan = sink1.get<gr::SpanReleasePolicy::ProcessAll>(n);
        bool equal    = sumSpan.size() == n && diffSpan.size() == n;
        for (std::size_t i = 0UZ; equal && i < n; ++i) {
            const auto a = sampleAt<std::complex<float>>(nextSample + i);
            const auto b = sampleAt<std::complex<float>>(nextSample + i + 7UZ);
            equal        = sumSpan[i] == a + b && diffSpan[i] == a - b;
        }
        nextSample += n;
        position.advance(n);
        return equal;
    }
};

// A stream-buffer span that exposes its storage only through its iterators, as the span concepts allow: it has no member data().
template<typename TElement>
struct IteratorOnlySpan {
    using value_type = std::remove_cv_t<TElement>;

    TElement*   first = nullptr;
    std::size_t count = 0UZ;

    [[nodiscard]] TElement*   begin() const noexcept { return first; }
    [[nodiscard]] TElement*   end() const noexcept { return first + count; }
    [[nodiscard]] std::size_t size() const noexcept { return count; }

    bool consume(std::size_t) noexcept { return true; }
    void publish(std::size_t) noexcept {}
    void publishTag(const gr::property_map&, std::size_t) noexcept {} // the non-const loop publishes a pending output tag through each writer span
};

} // namespace gr::qa_processone_pure

// SpanLike converts the span to std::span as an rvalue, which std::span accepts for a borrowed range; the span does not own its storage
namespace std::ranges {
template<typename TElement>
inline constexpr bool enable_borrowed_range<gr::qa_processone_pure::IteratorOnlySpan<TElement>> = true;
} // namespace std::ranges

namespace gr::qa_processone_pure {
static_assert(gr::ReaderSpanLike<IteratorOnlySpan<const std::complex<float>>>);
static_assert(gr::WriterSpanLike<IteratorOnlySpan<std::complex<float>>>);
template<typename T>
concept HasMemberData = requires(T& s) { s.data(); };
static_assert(!HasMemberData<IteratorOnlySpan<const std::complex<float>>>);
static_assert(!HasMemberData<IteratorOnlySpan<std::complex<float>>>);
} // namespace gr::qa_processone_pure

const boost::ut::suite<"processOne const loop"> processOnePureTests = [] {
    using namespace boost::ut;
    using namespace gr::qa_processone_pure;

    // sizes around the SIMD lane widths, with odd tails; each size runs twice on one block, the second chunk starting at a non-zero ring position
    const std::vector<std::size_t> chunkSizes = {1, 2, 3, 4, 5, 7, 8, 9, 15, 16, 17, 31, 32, 33, 63, 64, 65, 127, 128, 129, 1000};

    "complex<float> over a const processOne"_test = [&] {
        for (const std::size_t n : chunkSizes) {
            OneInOneOut<ComplexAffine, std::complex<float>> rig;
            for (int repeat = 0; repeat < 2; ++repeat) {
                std::vector<std::complex<float>> want;
                const auto                       got = rig.runChunk(n, [](std::complex<float> x) { return ComplexAffine{}.processOne(x); }, want);
                expect(eq(got.size(), n)) << "n =" << n;
                expect(got == want) << "output differs for n =" << n << "chunk" << repeat;
            }
            expect(eq(rig.position.wrapped > 0UZ, 2UZ * n > rig.position.capacity)) << "wrap reached for n =" << n;
            if (n == 1000UZ) {
                expect(rig.position.wrapped > 0UZ) << "the second chunk of 1000 samples wraps";
            }
        }
    };

    "aggregate over a const processOne"_test = [&] {
        for (const std::size_t n : chunkSizes) {
            OneInOneOut<PairAffine, Pair> rig;
            for (int repeat = 0; repeat < 2; ++repeat) {
                std::vector<Pair> want;
                const auto        got = rig.runChunk(n, [](const Pair& x) { return PairAffine{}.processOne(x); }, want);
                expect(eq(got.size(), n)) << "n =" << n;
                expect(got == want) << "output differs for n =" << n << "chunk" << repeat;
            }
            expect(eq(rig.position.wrapped > 0UZ, 2UZ * n > rig.position.capacity)) << "wrap reached for n =" << n;
            if (n == 1000UZ) {
                expect(rig.position.wrapped > 0UZ) << "the second chunk of 1000 samples wraps";
            }
        }
    };

    "two inputs and two outputs over a const processOne"_test = [&] {
        for (const std::size_t n : chunkSizes) {
            TwoInTwoOut rig;
            for (int repeat = 0; repeat < 2; ++repeat) {
                expect(rig.runChunk(n)) << "output differs for n =" << n << "chunk" << repeat;
            }
            expect(eq(rig.position.wrapped > 0UZ, 2UZ * n > rig.position.capacity)) << "wrap reached for n =" << n;
            if (n == 1000UZ) {
                expect(rig.position.wrapped > 0UZ) << "the second chunk of 1000 samples wraps";
            }
        }
    };

    "custom span without a member data() over a const processOne"_test = [] {
        constexpr std::size_t            n = 33UZ;
        std::vector<std::complex<float>> input(n);
        std::vector<std::complex<float>> output(n);
        for (std::size_t i = 0UZ; i < n; ++i) {
            input[i] = sampleAt<std::complex<float>>(i);
        }
        ComplexAffine block;
        auto          inputSpans  = std::tuple{IteratorOnlySpan<const std::complex<float>>{input.data(), n}};
        auto          outputSpans = std::tuple{IteratorOnlySpan<std::complex<float>>{output.data(), n}};
        expect(block.invokeProcessOnePure(inputSpans, outputSpans, n) == gr::work::Status::OK);
        for (std::size_t i = 0UZ; i < n; ++i) {
            expect(output[i] == block.processOne(input[i])) << "i =" << i;
        }
    };

    "custom span without a member data() over a non-const processOne"_test = [] {
        constexpr std::size_t            n = 33UZ;
        std::vector<std::complex<float>> input(n);
        std::vector<std::complex<float>> output(n);
        for (std::size_t i = 0UZ; i < n; ++i) {
            input[i] = sampleAt<std::complex<float>>(i);
        }
        RunningSum block;
        auto       inputSpans  = std::tuple{IteratorOnlySpan<const std::complex<float>>{input.data(), n}};
        auto       outputSpans = std::tuple{IteratorOnlySpan<std::complex<float>>{output.data(), n}};
        const auto result      = block.invokeProcessOneNonConst(inputSpans, outputSpans, n);
        expect(result.status == gr::work::Status::OK);
        expect(eq(result.processedOut, n));
        std::complex<float> sum{};
        for (std::size_t i = 0UZ; i < n; ++i) {
            sum += input[i];
            expect(output[i] == sum) << "i =" << i;
        }
    };
};

int main() { /* tests are statically executed */ }
