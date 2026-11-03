#include <boost/ut.hpp>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <format>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <gnuradio-4.0/Block.hpp>
#include <gnuradio-4.0/Graph.hpp>
#include <gnuradio-4.0/Scheduler.hpp>

namespace qa_drain {

constexpr std::size_t kBurst = 8UZ;

// publishes the whole burst and ends the stream in the same call, so every item and the end-of-stream
// marker reach the block downstream in a single delivery
struct BurstSource : gr::Block<BurstSource> {
    gr::PortOut<int> out;

    GR_MAKE_REFLECTABLE(BurstSource, out);

    gr::work::Status processBulk(gr::OutputSpanLike auto& outSpan) {
        if (outSpan.size() < kBurst) {
            outSpan.publish(0UZ);
            return gr::work::Status::INSUFFICIENT_OUTPUT_ITEMS;
        }
        for (std::size_t i = 0UZ; i < kBurst; ++i) {
            outSpan[i] = static_cast<int>(i);
        }
        outSpan.publish(kBurst);
        return gr::work::Status::DONE;
    }
};

// takes one item per call off an asynchronous input, the shape of a block that emits one record at a time:
// the items it was handed outlive the arrival of the end-of-stream marker behind them
struct OneAtATime : gr::Block<OneAtATime> {
    gr::PortIn<int, gr::Async>  in;
    gr::PortOut<int, gr::Async> out;

    GR_MAKE_REFLECTABLE(OneAtATime, in, out);

    std::size_t _nForwarded = 0UZ;

    gr::work::Status processBulk(gr::InputSpanLike auto& inSpan, gr::OutputSpanLike auto& outSpan) {
        if (inSpan.size() == 0UZ) {
            std::ignore = inSpan.consume(0UZ);
            outSpan.publish(0UZ);
            return gr::work::Status::INSUFFICIENT_INPUT_ITEMS;
        }
        if (outSpan.size() == 0UZ) {
            std::ignore = inSpan.consume(0UZ);
            outSpan.publish(0UZ);
            return gr::work::Status::INSUFFICIENT_OUTPUT_ITEMS;
        }
        outSpan[0UZ] = inSpan[0UZ];
        std::ignore  = inSpan.consume(1UZ);
        outSpan.publish(1UZ);
        _nForwarded++;
        return gr::work::Status::OK;
    }
};

// never takes what it was handed: the graph has to end anyway
struct StuckRelay : gr::Block<StuckRelay> {
    gr::PortIn<int, gr::Async>  in;
    gr::PortOut<int, gr::Async> out;

    GR_MAKE_REFLECTABLE(StuckRelay, in, out);

    std::size_t _nCalls = 0UZ;

    gr::work::Status processBulk(gr::InputSpanLike auto& inSpan, gr::OutputSpanLike auto& outSpan) {
        _nCalls++;
        std::ignore = inSpan.consume(0UZ);
        outSpan.publish(0UZ);
        return gr::work::Status::INSUFFICIENT_INPUT_ITEMS;
    }
};

constexpr std::size_t kItemsBeforeFailure = 2UZ;

// forwards a couple of items and then fails, leaving the rest of the burst in its input queue
struct FailingRelay : gr::Block<FailingRelay> {
    gr::PortIn<int, gr::Async>  in;
    gr::PortOut<int, gr::Async> out;

    GR_MAKE_REFLECTABLE(FailingRelay, in, out);

    std::size_t _nForwarded = 0UZ;

    gr::work::Status processBulk(gr::InputSpanLike auto& inSpan, gr::OutputSpanLike auto& outSpan) {
        if (_nForwarded >= kItemsBeforeFailure) {
            std::ignore = inSpan.consume(0UZ);
            outSpan.publish(0UZ);
            return gr::work::Status::ERROR;
        }
        if (inSpan.size() == 0UZ || outSpan.size() == 0UZ) {
            std::ignore = inSpan.consume(0UZ);
            outSpan.publish(0UZ);
            return gr::work::Status::INSUFFICIENT_INPUT_ITEMS;
        }
        outSpan[0UZ] = inSpan[0UZ];
        std::ignore  = inSpan.consume(1UZ);
        outSpan.publish(1UZ);
        _nForwarded++;
        return gr::work::Status::OK;
    }
};

constexpr std::size_t kWindow = 4UZ;

// needs a window of samples to make one item and takes one sample per call, so what it still owes the loop is
// known only where the call ends: it publishes that requirement as its input port minimum from processBulk().
// The window it cannot fill is the tail the end of the stream leaves, and the epilogue is where it says so.
struct WindowedRelay : gr::Block<WindowedRelay> {
    gr::PortIn<int>  in;
    gr::PortOut<int> out;

    GR_MAKE_REFLECTABLE(WindowedRelay, in, out);

    std::size_t _nShortCalls = 0UZ; // calls offered fewer samples than the minimum the previous call published
    std::size_t _tail        = 0UZ; // what the stream left that no window could be filled from
    bool        _epilogueRan = false;

    gr::work::Status processBulk(gr::InputSpanLike auto& inSpan, gr::OutputSpanLike auto& outSpan) {
        in.min_samples = kWindow;
        if (inSpan.size() < kWindow) {
            _nShortCalls++;
            std::ignore = inSpan.consume(0UZ);
            outSpan.publish(0UZ);
            return gr::work::Status::INSUFFICIENT_INPUT_ITEMS;
        }
        if (outSpan.size() == 0UZ) {
            std::ignore = inSpan.consume(0UZ);
            outSpan.publish(0UZ);
            return gr::work::Status::INSUFFICIENT_OUTPUT_ITEMS;
        }
        outSpan[0UZ] = inSpan[0UZ];
        std::ignore  = inSpan.consume(1UZ);
        outSpan.publish(1UZ);
        return gr::work::Status::OK;
    }

    gr::work::Status processEpilogue(gr::InputSpanLike auto& inSpan, gr::OutputSpanLike auto& outSpan) {
        _epilogueRan = true;
        _tail        = inSpan.size();
        outSpan.publish(0UZ);
        return gr::work::Status::OK;
    }
};

struct CountingSink : gr::Block<CountingSink> {
    gr::PortIn<int> in;

    GR_MAKE_REFLECTABLE(CountingSink, in);

    std::size_t _nReceived = 0UZ;

    void processOne(int) { _nReceived++; }
};

constexpr std::size_t kShortBranch = 10UZ;
constexpr std::size_t kLongBranch  = 1000UZ;
constexpr std::size_t kMaxPerCall  = 8UZ;

// publishes _nItems items, at most _maxPerCall per call, and ends its stream in the call that publishes the last one
struct CountedSource : gr::Block<CountedSource> {
    gr::PortOut<int> out;

    GR_MAKE_REFLECTABLE(CountedSource, out);

    std::size_t _nItems     = 0UZ;
    std::size_t _maxPerCall = 0UZ;
    std::size_t _nPublished = 0UZ;

    gr::work::Status processBulk(gr::OutputSpanLike auto& outSpan) {
        const std::size_t n = std::min({outSpan.size(), _maxPerCall, _nItems - _nPublished});
        for (std::size_t i = 0UZ; i < n; ++i) {
            outSpan[i] = static_cast<int>(_nPublished + i);
        }
        outSpan.publish(n);
        _nPublished += n;
        return _nPublished == _nItems ? gr::work::Status::DONE : gr::work::Status::OK;
    }
};

// takes everything its two asynchronous inputs hold and counts it per input
struct AsyncPairSink : gr::Block<AsyncPairSink> {
    gr::PortIn<int, gr::Async> first;
    gr::PortIn<int, gr::Async> second;

    GR_MAKE_REFLECTABLE(AsyncPairSink, first, second);

    std::size_t _nFirst  = 0UZ;
    std::size_t _nSecond = 0UZ;

    gr::work::Status processBulk(gr::InputSpanLike auto& firstSpan, gr::InputSpanLike auto& secondSpan) {
        const std::size_t nFirst  = firstSpan.size();
        const std::size_t nSecond = secondSpan.size();
        std::ignore               = firstSpan.consume(nFirst);
        std::ignore               = secondSpan.consume(nSecond);
        _nFirst += nFirst;
        _nSecond += nSecond;
        return nFirst + nSecond > 0UZ ? gr::work::Status::OK : gr::work::Status::INSUFFICIENT_INPUT_ITEMS;
    }
};

// counts like AsyncPairSink and ends its own stream at the end of its first input
struct FirstEndSink : gr::Block<FirstEndSink> {
    gr::PortIn<int, gr::Async> first;
    gr::PortIn<int, gr::Async> second;

    GR_MAKE_REFLECTABLE(FirstEndSink, first, second);

    std::size_t _nFirst  = 0UZ;
    std::size_t _nSecond = 0UZ;

    gr::work::Status processBulk(gr::InputSpanLike auto& firstSpan, gr::InputSpanLike auto& secondSpan) {
        if (firstSpan.size() == 0UZ && gr::samples_to_eos_tag(first) == 0UZ) {
            std::ignore = firstSpan.consume(0UZ);
            std::ignore = secondSpan.consume(0UZ);
            this->requestStop();
            return gr::work::Status::OK;
        }
        const std::size_t nFirst  = firstSpan.size();
        const std::size_t nSecond = secondSpan.size();
        std::ignore               = firstSpan.consume(nFirst);
        std::ignore               = secondSpan.consume(nSecond);
        _nFirst += nFirst;
        _nSecond += nSecond;
        return nFirst + nSecond > 0UZ ? gr::work::Status::OK : gr::work::Status::INSUFFICIENT_INPUT_ITEMS;
    }
};

// takes everything a synchronous and an asynchronous input hold and counts it per input
struct MixedPairSink : gr::Block<MixedPairSink> {
    gr::PortIn<int>            data;
    gr::PortIn<int, gr::Async> aux;

    GR_MAKE_REFLECTABLE(MixedPairSink, data, aux);

    std::size_t _nData              = 0UZ;
    std::size_t _nAux               = 0UZ;
    std::size_t _nCallsAfterDataEnd = 0UZ; // calls that found the synchronous input at its end

    gr::work::Status processBulk(gr::InputSpanLike auto& dataSpan, gr::InputSpanLike auto& auxSpan) {
        if (dataSpan.size() == 0UZ && gr::samples_to_eos_tag(data) == 0UZ) {
            _nCallsAfterDataEnd++;
        }
        const std::size_t nData = dataSpan.size();
        const std::size_t nAux  = auxSpan.size();
        std::ignore             = dataSpan.consume(nData);
        std::ignore             = auxSpan.consume(nAux);
        _nData += nData;
        _nAux += nAux;
        return nData + nAux > 0UZ ? gr::work::Status::OK : gr::work::Status::INSUFFICIENT_INPUT_ITEMS;
    }
};

constexpr std::size_t kStreamItems = 1000UZ; // one source's stream, fed to a block over a direct edge and a slow branch

// forwards one sample per call. A branch through it delivers each sample later than a direct edge from the same source.
struct OneSamplePerCall : gr::Block<OneSamplePerCall> {
    gr::PortIn<int>  in;
    gr::PortOut<int> out;

    GR_MAKE_REFLECTABLE(OneSamplePerCall, in, out);

    gr::work::Status processBulk(gr::InputSpanLike auto& inSpan, gr::OutputSpanLike auto& outSpan) {
        outSpan[0UZ] = inSpan[0UZ];
        std::ignore  = inSpan.consume(1UZ);
        outSpan.publish(1UZ);
        return gr::work::Status::OK;
    }
};

// takes what its two synchronous inputs hold together and counts the pairs whose values differ
struct SyncPairSink : gr::Block<SyncPairSink> {
    gr::PortIn<int> first;
    gr::PortIn<int> second;

    GR_MAKE_REFLECTABLE(SyncPairSink, first, second);

    std::size_t _nReceived          = 0UZ;
    std::size_t _nMismatched        = 0UZ;
    std::size_t _nCallsFirstEndOnly = 0UZ; // calls that found the end of the first input in view and none on the second

    gr::work::Status processBulk(gr::InputSpanLike auto& firstSpan, gr::InputSpanLike auto& secondSpan) {
        if (gr::samples_to_eos_tag(first).has_value() && !gr::samples_to_eos_tag(second).has_value()) {
            _nCallsFirstEndOnly++;
        }
        const std::size_t n = firstSpan.size();
        for (std::size_t i = 0UZ; i < n; ++i) {
            if (firstSpan[i] != secondSpan[i]) {
                _nMismatched++;
            }
        }
        std::ignore = firstSpan.consume(n);
        std::ignore = secondSpan.consume(n);
        _nReceived += n;
        return gr::work::Status::OK;
    }
};

// needs a window of samples to make one item and takes one sample per call, and keeps the default input minimum of
// one: at the end of a finite stream it is offered the last samples on every call and never takes them
struct HoldBackRelay : gr::Block<HoldBackRelay> {
    gr::PortIn<int>  in;
    gr::PortOut<int> out;

    GR_MAKE_REFLECTABLE(HoldBackRelay, in, out);

    gr::work::Status processBulk(gr::InputSpanLike auto& inSpan, gr::OutputSpanLike auto& outSpan) {
        if (inSpan.size() < kWindow) {
            std::ignore = inSpan.consume(0UZ);
            outSpan.publish(0UZ);
            return gr::work::Status::INSUFFICIENT_INPUT_ITEMS;
        }
        if (outSpan.size() == 0UZ) {
            std::ignore = inSpan.consume(0UZ);
            outSpan.publish(0UZ);
            return gr::work::Status::INSUFFICIENT_OUTPUT_ITEMS;
        }
        outSpan[0UZ] = inSpan[0UZ];
        std::ignore  = inSpan.consume(1UZ);
        outSpan.publish(1UZ);
        return gr::work::Status::OK;
    }
};

// publishes one and a half of its output ring as fast as the ring takes them and then ends its stream
struct RingAndAHalfSource : gr::Block<RingAndAHalfSource> {
    gr::PortOut<int> out;

    GR_MAKE_REFLECTABLE(RingAndAHalfSource, out);

    std::size_t _nItems     = 0UZ;
    std::size_t _nPublished = 0UZ;

    gr::work::Status processBulk(gr::OutputSpanLike auto& outSpan) {
        if (_nItems == 0UZ) {
            _nItems = out.bufferSize() + out.bufferSize() / 2UZ;
        }
        const std::size_t n = std::min(outSpan.size(), _nItems - _nPublished);
        for (std::size_t i = 0UZ; i < n; ++i) {
            outSpan[i] = static_cast<int>(_nPublished + i);
        }
        outSpan.publish(n);
        _nPublished += n;
        return _nPublished == _nItems ? gr::work::Status::DONE : gr::work::Status::OK;
    }
};

struct Passthrough : gr::Block<Passthrough> {
    gr::PortIn<int>  in;
    gr::PortOut<int> out;

    GR_MAKE_REFLECTABLE(Passthrough, in, out);

    [[nodiscard]] int processOne(int value) const noexcept { return value; }
};

constexpr gr::Size_t kInterpolation = 2U;

// writes each input sample kInterpolation times. One input sample needs room for kInterpolation output samples.
struct Interpolator : gr::Block<Interpolator, gr::Resampling<1U, kInterpolation, true>> {
    gr::PortIn<int>  in;
    gr::PortOut<int> out;

    GR_MAKE_REFLECTABLE(Interpolator, in, out);

    gr::work::Status processBulk(gr::InputSpanLike auto& inSpan, gr::OutputSpanLike auto& outSpan) {
        const std::size_t n = inSpan.size();
        for (std::size_t i = 0UZ; i < n; ++i) {
            for (std::size_t k = 0UZ; k < kInterpolation; ++k) {
                outSpan[i * kInterpolation + k] = inSpan[i];
            }
        }
        std::ignore = inSpan.consume(n);
        outSpan.publish(n * kInterpolation);
        return gr::work::Status::OK;
    }
};

constexpr gr::Size_t  kDecimation = 4U;
constexpr std::size_t kTail       = 2UZ; // fewer samples than one chunk: the end of the stream hands them to the epilogue
static_assert(kTail < kDecimation);

// publishes kDecimation times its output ring and kTail more as fast as the ring takes them, then ends its stream
struct TailSource : gr::Block<TailSource> {
    gr::PortOut<int> out;

    GR_MAKE_REFLECTABLE(TailSource, out);

    std::size_t _nItems     = 0UZ;
    std::size_t _nPublished = 0UZ;

    gr::work::Status processBulk(gr::OutputSpanLike auto& outSpan) {
        if (_nItems == 0UZ) {
            _nItems = kDecimation * out.bufferSize() + kTail;
        }
        const std::size_t n = std::min(outSpan.size(), _nItems - _nPublished);
        for (std::size_t i = 0UZ; i < n; ++i) {
            outSpan[i] = static_cast<int>(_nPublished + i);
        }
        outSpan.publish(n);
        _nPublished += n;
        return _nPublished == _nItems ? gr::work::Status::DONE : gr::work::Status::OK;
    }
};

// keeps the first sample of each chunk. Its epilogue makes one sample from the partial chunk when its span has room.
struct TailDecimator : gr::Block<TailDecimator, gr::Resampling<kDecimation, 1U, true>> {
    gr::PortIn<int>  in;
    gr::PortOut<int> out;

    GR_MAKE_REFLECTABLE(TailDecimator, in, out);

    std::size_t _epilogueSpan = 0UZ; // the output span the epilogue was handed

    gr::work::Status processBulk(gr::InputSpanLike auto& inSpan, gr::OutputSpanLike auto& outSpan) {
        for (std::size_t i = 0UZ; i < outSpan.size(); ++i) {
            outSpan[i] = inSpan[i * kDecimation];
        }
        return gr::work::Status::OK;
    }

    gr::work::Status processEpilogue(gr::InputSpanLike auto& inSpan, gr::OutputSpanLike auto& outSpan) {
        _epilogueSpan = outSpan.size();
        if (inSpan.size() > 0UZ && outSpan.size() > 0UZ) {
            outSpan[0UZ] = inSpan[0UZ];
            outSpan.publish(1UZ);
        } else {
            outSpan.publish(0UZ);
        }
        return gr::work::Status::OK;
    }
};

// keeps the first sample of each chunk and defines no epilogue. It requests its stop when stop_now turns true.
struct ChunkDecimator : gr::Block<ChunkDecimator, gr::Resampling<kDecimation, 1U>> {
    gr::PortIn<int>  in;
    gr::PortOut<int> out;
    bool             stop_now = false;

    GR_MAKE_REFLECTABLE(ChunkDecimator, in, out, stop_now);

    gr::work::Status processBulk(gr::InputSpanLike auto& inSpan, gr::OutputSpanLike auto& outSpan) {
        const std::size_t chunk = static_cast<std::size_t>(this->input_chunk_size);
        for (std::size_t i = 0UZ; i < outSpan.size(); ++i) {
            outSpan[i] = inSpan[i * chunk];
        }
        return gr::work::Status::OK;
    }

    void settingsChanged(const gr::property_map& /*oldSettings*/, const gr::property_map& /*newSettings*/) {
        if (stop_now) {
            this->requestStop();
        }
    }
};

// keeps the first sample of each chunk. Its stride of one sample makes its chunks overlap. It records the highest
// sample value it was handed.
struct StridedDecimator : gr::Block<StridedDecimator, gr::Resampling<kDecimation, 1U>, gr::Stride<1U>> {
    gr::PortIn<int>  in;
    gr::PortOut<int> out;

    GR_MAKE_REFLECTABLE(StridedDecimator, in, out);

    int _highestHanded = -1;

    gr::work::Status processBulk(gr::InputSpanLike auto& inSpan, gr::OutputSpanLike auto& outSpan) {
        for (std::size_t i = 0UZ; i < inSpan.size(); ++i) {
            _highestHanded = std::max(_highestHanded, inSpan[i]);
        }
        const std::size_t chunk = static_cast<std::size_t>(this->input_chunk_size);
        for (std::size_t i = 0UZ; i < outSpan.size(); ++i) {
            outSpan[i] = inSpan[i * chunk];
        }
        return gr::work::Status::OK;
    }
};

// takes what the two inputs of its port collection hold together and counts it
struct CollectionPairSink : gr::Block<CollectionPairSink> {
    std::vector<gr::PortIn<int>> in;

    GR_MAKE_REFLECTABLE(CollectionPairSink, in);

    std::size_t _nReceived = 0UZ;

    explicit CollectionPairSink(gr::property_map init = {}) : gr::Block<CollectionPairSink>(std::move(init)) { in.resize(2UZ); }

    template<gr::InputSpanLike TInSpan>
    gr::work::Status processBulk(const std::span<TInSpan>& inSpans) {
        _nReceived += inSpans[0UZ].size();
        return gr::work::Status::OK;
    }
};

// takes everything its asynchronous input holds and counts it
struct AsyncCountingSink : gr::Block<AsyncCountingSink> {
    gr::PortIn<int, gr::Async> in;

    GR_MAKE_REFLECTABLE(AsyncCountingSink, in);

    std::size_t _nReceived = 0UZ;

    gr::work::Status processBulk(gr::InputSpanLike auto& inSpan) {
        const std::size_t n = inSpan.size();
        std::ignore         = inSpan.consume(n);
        _nReceived += n;
        return n > 0UZ ? gr::work::Status::OK : gr::work::Status::INSUFFICIENT_INPUT_ITEMS;
    }
};

// consumes at most kConsumed samples per call and records how many it was handed. With _lowerMax set, it lowers
// max_samples to kLoweredMax after its first call. With _minAfterSecondCall set, it sets max_samples to _defaultMax and
// min_samples to _minAfterSecondCall after its second call.
struct PartialConsumer : gr::Block<PartialConsumer> {
    static constexpr std::size_t kConsumed   = 2UZ;
    static constexpr std::size_t kLoweredMax = 2UZ;

    gr::PortIn<int> in;

    GR_MAKE_REFLECTABLE(PartialConsumer, in);

    std::vector<std::size_t> _handed;
    std::size_t              _defaultMax         = 0UZ;
    std::size_t              _minAfterSecondCall = 0UZ;
    bool                     _lowerMax           = false;

    gr::work::Status processBulk(gr::InputSpanLike auto& inSpan) {
        _handed.push_back(inSpan.size());
        std::ignore = inSpan.consume(std::min(inSpan.size(), kConsumed));
        if (_lowerMax && _handed.size() == 1UZ) {
            in.max_samples = kLoweredMax;
        }
        if (_handed.size() == 2UZ && _minAfterSecondCall > 0UZ) {
            in.max_samples = _defaultMax;
            in.min_samples = _minAfterSecondCall;
        }
        return gr::work::Status::OK;
    }
};

// consumes one item per call and returns INSUFFICIENT_INPUT_ITEMS when it is handed none. It records how many items
// it was handed and how many it consumed.
struct AsyncPartialSink : gr::Block<AsyncPartialSink> {
    gr::PortIn<int, gr::Async> in;

    GR_MAKE_REFLECTABLE(AsyncPartialSink, in);

    std::size_t _nHanded   = 0UZ;
    std::size_t _nConsumed = 0UZ;

    gr::work::Status processBulk(gr::InputSpanLike auto& inSpan) {
        _nHanded += inSpan.size();
        const std::size_t n = std::min(inSpan.size(), 1UZ);
        std::ignore         = inSpan.consume(n);
        _nConsumed += n;
        return n > 0UZ ? gr::work::Status::OK : gr::work::Status::INSUFFICIENT_INPUT_ITEMS;
    }
};

// has a stride of 3 samples and no Resampling<>, so each call is handed all the input may give and the block's own
// consume is the stride. It records how many samples it was handed. After its first call it lowers max_samples to
// kLoweredMax, and after its second it sets max_samples to _defaultMax and min_samples to _minAfterSecondCall.
struct StrideSink : gr::Block<StrideSink, gr::Stride<3U>> {
    static constexpr std::size_t kLoweredMax = 2UZ;

    gr::PortIn<int> in;

    GR_MAKE_REFLECTABLE(StrideSink, in);

    std::vector<std::size_t> _handed;
    std::size_t              _defaultMax         = 0UZ;
    std::size_t              _minAfterSecondCall = 0UZ;

    gr::work::Status processBulk(gr::InputSpanLike auto& inSpan) {
        _handed.push_back(inSpan.size());
        if (_handed.size() == 1UZ) {
            in.max_samples = kLoweredMax;
        }
        if (_handed.size() == 2UZ) {
            in.max_samples = _defaultMax;
            in.min_samples = _minAfterSecondCall;
        }
        return gr::work::Status::OK;
    }
};

// publishes its output ring and one sample more as fast as the ring takes them, then ends its stream
struct RingPlusOneSource : gr::Block<RingPlusOneSource> {
    gr::PortOut<int> out;

    GR_MAKE_REFLECTABLE(RingPlusOneSource, out);

    std::size_t _nItems     = 0UZ;
    std::size_t _nPublished = 0UZ;

    gr::work::Status processBulk(gr::OutputSpanLike auto& outSpan) {
        if (_nItems == 0UZ) {
            _nItems = out.bufferSize() + 1UZ;
        }
        const std::size_t n = std::min(outSpan.size(), _nItems - _nPublished);
        for (std::size_t i = 0UZ; i < n; ++i) {
            outSpan[i] = static_cast<int>(_nPublished + i);
        }
        outSpan.publish(n);
        _nPublished += n;
        return _nPublished == _nItems ? gr::work::Status::DONE : gr::work::Status::OK;
    }
};

// forwards the first sample it is handed and declines every later one. Its epilogue copies as much of its tail as its
// span holds.
struct FirstSampleRelay : gr::Block<FirstSampleRelay> {
    gr::PortIn<int>  in;
    gr::PortOut<int> out;

    GR_MAKE_REFLECTABLE(FirstSampleRelay, in, out);

    bool        _forwarded    = false;
    std::size_t _epilogueTail = 0UZ; // the input span the epilogue was handed
    std::size_t _epilogueSpan = 0UZ; // the output span the epilogue was handed

    gr::work::Status processBulk(gr::InputSpanLike auto& inSpan, gr::OutputSpanLike auto& outSpan) {
        if (_forwarded || inSpan.size() == 0UZ || outSpan.size() == 0UZ) {
            std::ignore = inSpan.consume(0UZ);
            outSpan.publish(0UZ);
            return gr::work::Status::INSUFFICIENT_INPUT_ITEMS;
        }
        _forwarded   = true;
        outSpan[0UZ] = inSpan[0UZ];
        std::ignore  = inSpan.consume(1UZ);
        outSpan.publish(1UZ);
        return gr::work::Status::OK;
    }

    gr::work::Status processEpilogue(gr::InputSpanLike auto& inSpan, gr::OutputSpanLike auto& outSpan) {
        _epilogueTail       = inSpan.size();
        _epilogueSpan       = outSpan.size();
        const std::size_t n = std::min(inSpan.size(), outSpan.size());
        for (std::size_t i = 0UZ; i < n; ++i) {
            outSpan[i] = inSpan[i];
        }
        outSpan.publish(n);
        return gr::work::Status::OK;
    }
};

// writes each input sample kInterpolation times and requests its stop when stop_now turns true. Its epilogue writes
// the copies of as many tail samples as its span holds. With _wholeTailOnly, it writes the copies of the whole tail
// or nothing.
struct TailInterpolator : gr::Block<TailInterpolator, gr::Resampling<1U, kInterpolation, true>> {
    gr::PortIn<int>  in;
    gr::PortOut<int> out;
    bool             stop_now = false;

    GR_MAKE_REFLECTABLE(TailInterpolator, in, out, stop_now);

    bool        _wholeTailOnly   = false;
    std::size_t _epilogueTail    = 0UZ; // the input samples the epilogue was handed
    std::size_t _epilogueSpan    = 0UZ; // the output span the epilogue was handed
    std::size_t _epilogueWritten = 0UZ;

    void settingsChanged(const gr::property_map& /*oldSettings*/, const gr::property_map& /*newSettings*/) {
        if (stop_now) {
            this->requestStop();
        }
    }

    gr::work::Status processBulk(gr::InputSpanLike auto& inSpan, gr::OutputSpanLike auto& outSpan) {
        for (std::size_t i = 0UZ; i < inSpan.size(); ++i) {
            for (std::size_t k = 0UZ; k < kInterpolation; ++k) {
                outSpan[i * kInterpolation + k] = inSpan[i];
            }
        }
        return gr::work::Status::OK;
    }

    gr::work::Status processEpilogue(gr::InputSpanLike auto& inSpan, gr::OutputSpanLike auto& outSpan) {
        _epilogueTail = inSpan.size();
        _epilogueSpan = outSpan.size();
        std::size_t n = std::min(inSpan.size(), outSpan.size() / kInterpolation);
        if (_wholeTailOnly && n < inSpan.size()) {
            n = 0UZ;
        }
        for (std::size_t i = 0UZ; i < n; ++i) {
            for (std::size_t k = 0UZ; k < kInterpolation; ++k) {
                outSpan[i * kInterpolation + k] = inSpan[i];
            }
        }
        outSpan.publish(n * kInterpolation);
        _epilogueWritten = n * kInterpolation;
        return gr::work::Status::OK;
    }
};

// connects the block between the two ports and returns the size of its output ring
[[nodiscard]] std::size_t connectBetween(TailInterpolator& block, gr::PortOut<int>& upstream, gr::PortIn<int>& downstream) {
    using namespace boost::ut;
    expect(fatal(upstream.connect(block.in).has_value()));
    expect(fatal(block.out.connect(downstream).has_value()));
    block.init(std::make_shared<gr::Sequence>());
    return block.out.bufferSize();
}

// publishes n samples on the port
void publishSamples(gr::PortOut<int>& port, std::size_t n) {
    auto span = port.reserve<gr::SpanReleasePolicy::ProcessAll>(n);
    std::ranges::fill(span, 1);
}

// declines every chunk. A stream that ends leaves it draining until it stalls. Its epilogue fills as much of its span
// as its tail's copies need.
struct DecliningInterpolator : gr::Block<DecliningInterpolator, gr::Resampling<1U, kInterpolation, true>> {
    gr::PortIn<int>  in;
    gr::PortOut<int> out;

    GR_MAKE_REFLECTABLE(DecliningInterpolator, in, out);

    std::size_t _epilogueSpan = 0UZ; // the output span the epilogue was handed

    gr::work::Status processBulk(gr::InputSpanLike auto& inSpan, gr::OutputSpanLike auto& outSpan) {
        std::ignore = inSpan.consume(0UZ);
        outSpan.publish(0UZ);
        return gr::work::Status::INSUFFICIENT_INPUT_ITEMS;
    }

    gr::work::Status processEpilogue(gr::InputSpanLike auto& inSpan, gr::OutputSpanLike auto& outSpan) {
        _epilogueSpan       = outSpan.size();
        const std::size_t n = std::min(inSpan.size() * kInterpolation, outSpan.size());
        std::fill_n(outSpan.begin(), n, 1);
        outSpan.publish(n);
        return gr::work::Status::OK;
    }
};

// copies its input to two outputs and requests its stop when stop_now turns true. Its epilogue copies as many tail
// samples as its spans hold.
struct TwoOutputTail : gr::Block<TwoOutputTail> {
    gr::PortIn<int>  in;
    gr::PortOut<int> first;
    gr::PortOut<int> second;
    bool             stop_now = false;

    GR_MAKE_REFLECTABLE(TwoOutputTail, in, first, second, stop_now);

    bool        _epilogueRan = false;
    std::size_t _firstSpan   = 0UZ; // the output spans the epilogue was handed
    std::size_t _secondSpan  = 0UZ;

    void settingsChanged(const gr::property_map& /*oldSettings*/, const gr::property_map& /*newSettings*/) {
        if (stop_now) {
            this->requestStop();
        }
    }

    gr::work::Status processBulk(gr::InputSpanLike auto& inSpan, gr::OutputSpanLike auto& firstSpan, gr::OutputSpanLike auto& secondSpan) {
        std::ranges::copy(inSpan, firstSpan.begin());
        std::ranges::copy(inSpan, secondSpan.begin());
        return gr::work::Status::OK;
    }

    gr::work::Status processEpilogue(gr::InputSpanLike auto& inSpan, gr::OutputSpanLike auto& firstSpan, gr::OutputSpanLike auto& secondSpan) {
        _epilogueRan        = true;
        _firstSpan          = firstSpan.size();
        _secondSpan         = secondSpan.size();
        const std::size_t n = std::min({inSpan.size(), firstSpan.size(), secondSpan.size()});
        std::copy_n(inSpan.begin(), n, firstSpan.begin());
        std::copy_n(inSpan.begin(), n, secondSpan.begin());
        firstSpan.publish(n);
        secondSpan.publish(n);
        return gr::work::Status::OK;
    }
};

// the stream index of the first end-of-stream tag the port holds
[[nodiscard]] std::optional<std::size_t> endOfStreamIndex(gr::PortIn<int>& port) {
    for (const gr::Tag& tag : port.tagReader().get()) {
        if (tag.map.contains(gr::tag::END_OF_STREAM.key())) {
            return tag.index;
        }
    }
    return std::nullopt;
}

constexpr std::size_t kDrainStallCalls = 1024UZ; // idle calls after which a block with its input's end in view ends
constexpr std::size_t kLateCalls       = 2UZ * kDrainStallCalls;

// takes nothing on its first _nLateCalls calls and everything after them, like a consumer busy for a long time in one
// call. With _nTakenWhenFull set, it takes that many items the first time it finds its ring full and leaves its
// producer that much room. It counts the calls it declines after the source has published its last item.
struct LateSink : gr::Block<LateSink> {
    gr::PortIn<int> in;

    GR_MAKE_REFLECTABLE(LateSink, in);

    const RingAndAHalfSource* _source         = nullptr;
    std::size_t               _nTakenWhenFull = 0UZ;
    std::size_t               _nLateCalls     = kLateCalls;

    bool        _tookWhenFull              = false;
    std::size_t _nCalls                    = 0UZ;
    std::size_t _ringSize                  = 0UZ;
    std::size_t _nLargestDeclined          = 0UZ; // the most items one declined call was offered
    std::size_t _nDeclinedAfterSourceEnded = 0UZ;
    std::size_t _nReceived                 = 0UZ;

    gr::work::Status processBulk(gr::InputSpanLike auto& inSpan) {
        _nCalls++;
        _ringSize = in.bufferSize();
        if (_nCalls <= _nLateCalls) {
            if (_nTakenWhenFull > 0UZ && !_tookWhenFull && inSpan.size() == _ringSize) {
                _tookWhenFull = true;
                _nReceived += _nTakenWhenFull;
                std::ignore = inSpan.consume(_nTakenWhenFull);
                return gr::work::Status::OK;
            }
            _nLargestDeclined = std::max(_nLargestDeclined, inSpan.size());
            if (_source != nullptr && _source->_nItems > 0UZ && _source->_nPublished == _source->_nItems) {
                _nDeclinedAfterSourceEnded++;
            }
            std::ignore = inSpan.consume(0UZ);
            return gr::work::Status::INSUFFICIENT_INPUT_ITEMS;
        }
        _nReceived += inSpan.size();
        std::ignore = inSpan.consume(inSpan.size());
        return gr::work::Status::OK;
    }
};

// returns INSUFFICIENT_OUTPUT_ITEMS with nothing taken on its first kLateCalls calls and takes everything after them,
// like a sink that waits on a device to accept samples at the device's own pace
struct DeviceSink : gr::Block<DeviceSink> {
    gr::PortIn<int> in;

    GR_MAKE_REFLECTABLE(DeviceSink, in);

    std::size_t _nCalls              = 0UZ;
    std::size_t _nWaitsWithEndInView = 0UZ;
    std::size_t _nReceived           = 0UZ;

    gr::work::Status processBulk(gr::InputSpanLike auto& inSpan) {
        _nCalls++;
        if (_nCalls <= kLateCalls) {
            if (gr::samples_to_eos_tag(in).has_value()) {
                _nWaitsWithEndInView++;
            }
            std::ignore = inSpan.consume(0UZ);
            return gr::work::Status::INSUFFICIENT_OUTPUT_ITEMS;
        }
        _nReceived += inSpan.size();
        std::ignore = inSpan.consume(inSpan.size());
        return gr::work::Status::OK;
    }
};

constexpr std::string_view kDrainStallEndpoint = "drain stall";

// takes every message waiting on the port and returns the drain-stall notifications the named block sent
[[nodiscard]] std::vector<gr::Message> takeDrainStallReports(gr::MsgPortIn& port, std::string_view blockName) {
    std::vector<gr::Message> reports;
    auto                     messages = port.streamReader().get();
    for (const gr::Message& message : messages) {
        if (message.cmd == gr::message::Command::Notify && message.serviceName == blockName && message.endpoint == kDrainStallEndpoint && message.data.has_value()) {
            reports.push_back(message);
        }
    }
    std::ignore = messages.consume(messages.size());
    return reports;
}

constexpr std::string_view kDroppedTailEndpoint = "dropped tail";

// takes every message waiting on the port and returns the dropped-tail notifications the named block sent
[[nodiscard]] std::vector<gr::Message> takeDroppedTailReports(gr::MsgPortIn& port, std::string_view blockName) {
    std::vector<gr::Message> reports;
    auto                     messages = port.streamReader().get();
    for (const gr::Message& message : messages) {
        if (message.cmd == gr::message::Command::Notify && message.serviceName == blockName && message.endpoint == kDroppedTailEndpoint && message.data.has_value()) {
            reports.push_back(message);
        }
    }
    std::ignore = messages.consume(messages.size());
    return reports;
}

[[nodiscard]] std::size_t reportedValue(const gr::Message& report, std::string_view key) { return report.data->at(std::pmr::string(key)).value_or<gr::Size_t>(0U); }

// the text the report carries under the key, or an empty string
[[nodiscard]] std::string reportedText(const gr::Message& report, std::string_view key) {
    const auto entry = report.data->find(std::pmr::string(key));
    return entry == report.data->end() ? std::string() : entry->second.value_or(std::string());
}

using SerialScheduler = gr::scheduler::Simple<gr::scheduler::ExecutionPolicy::singleThreaded>;

constexpr auto kRunBound = std::chrono::seconds(5);

// The cases whose consumer declines for kLateCalls calls run this long at most. The scheduler sleeps on each idle pass.
constexpr auto kSlowConsumerBound = std::chrono::seconds(30);

// runAndWait() on its own thread with a deadline, so a graph that fails to end fails the assertion
// instead of hanging ctest
template<typename TScheduler>
[[nodiscard]] bool runWithin(TScheduler& scheduler, std::chrono::milliseconds bound) {
    std::mutex              mutex;
    std::condition_variable finished;
    bool                    returned = false;

    std::thread runner([&scheduler, &mutex, &finished, &returned] {
        std::ignore = scheduler.runAndWait();
        {
            std::lock_guard lock(mutex);
            returned = true;
        }
        finished.notify_one();
    });

    bool inTime = false;
    {
        std::unique_lock lock(mutex);
        inTime = finished.wait_for(lock, bound, [&returned] { return returned; });
    }
    if (!inTime) {
        scheduler.requestStop(); // release the run loop so the process can still exit
    }
    runner.join();
    return inTime;
}

// runs the graph on the scheduler and returns the dropped-tail notifications the named block sent to fromScheduler
[[nodiscard]] std::vector<gr::Message> runAndTakeDroppedTailReports(SerialScheduler& scheduler, gr::MsgPortIn& fromScheduler, gr::Graph&& flow, std::string_view blockName) {
    using namespace boost::ut;
    expect(scheduler.msgOut.connect(fromScheduler).has_value());
    expect(scheduler.exchange(std::move(flow)).has_value());
    expect(runWithin(scheduler, std::chrono::duration_cast<std::chrono::milliseconds>(kRunBound))) << "the graph did not end";
    return takeDroppedTailReports(fromScheduler, blockName);
}

// The relay holds its full input ring at the end of its stream. The decimator holds the relay's first sample, less than
// one chunk. The relay's output then has room for one sample less than the relay's tail. Its epilogue waits for that
// room until the wait stalls and is then handed the room the output has.
template<typename TScheduler>
void expectEpilogueWaitEndsUnderScheduler() {
    using namespace boost::ut;
    gr::Graph flow;
    auto&     source    = flow.emplaceBlock<RingPlusOneSource>();
    auto&     relay     = flow.emplaceBlock<FirstSampleRelay>();
    auto&     decimator = flow.emplaceBlock<TailDecimator>();
    auto&     sink      = flow.emplaceBlock<CountingSink>();
    expect(flow.connect<"out", "in">(source, relay).has_value());
    expect(flow.connect<"out", "in">(relay, decimator).has_value());
    expect(flow.connect<"out", "in">(decimator, sink).has_value());

    TScheduler scheduler;
    // a worker parked once per waiting call would take longer than the bound
    const std::size_t parkedWaitMs = std::size_t{scheduler.timeout_ms.value} * kDrainStallCalls;
    const std::size_t boundMs      = static_cast<std::size_t>(std::chrono::duration_cast<std::chrono::milliseconds>(kRunBound).count());
    expect(fatal(gt(parkedWaitMs, boundMs)));
    expect(scheduler.exchange(std::move(flow)).has_value());
    expect(runWithin(scheduler, std::chrono::duration_cast<std::chrono::milliseconds>(kRunBound))) << "the epilogue's wait for room held the graph open";

    const std::size_t ring = relay.out.bufferSize();
    expect(eq(relay._epilogueTail, source.out.bufferSize())) << "the relay's input ring was not full at its end";
    expect(eq(relay._epilogueSpan, ring - 1UZ)) << "the epilogue was not handed the room the output has";
    const std::size_t nDecimated = 1UZ + relay._epilogueSpan;
    expect(eq(sink._nReceived, (nDecimated + kDecimation - 1UZ) / kDecimation)) << "the samples the relay wrote did not all reach the sink";
}

// a source whose per-sample processOne publishes a tag at sample _tagAt of a call and requests a stop at sample _stopAt
struct MarkingSource : gr::Block<MarkingSource> {
    gr::PortOut<int> out;

    GR_MAKE_REFLECTABLE(MarkingSource, out);

    static constexpr std::size_t kNever = std::numeric_limits<std::size_t>::max();

    std::size_t _tagAt  = kNever;
    std::size_t _stopAt = kNever;
    std::size_t _next   = 0UZ;

    int processOne() {
        const std::size_t sample = _next++;
        if (sample == _tagAt) {
            this->publishTag(gr::property_map{{"marker", 1}}, 0UZ);
        }
        if (sample == _stopAt) {
            this->requestStop();
        }
        return static_cast<int>(sample);
    }
};

constexpr std::size_t kMarkedCall = 4096UZ;

} // namespace qa_drain

const boost::ut::suite<"end-of-stream drain"> drainTests = [] {
    using namespace boost::ut;
    using enum gr::lifecycle::State;

    "a block emitting one item per call is given every item it holds"_test = [] {
        gr::Graph flow;
        auto&     source = flow.emplaceBlock<qa_drain::BurstSource>();
        auto&     relay  = flow.emplaceBlock<qa_drain::OneAtATime>();
        auto&     sink   = flow.emplaceBlock<qa_drain::CountingSink>();
        expect(flow.connect<"out", "in">(source, relay).has_value());
        expect(flow.connect<"out", "in">(relay, sink).has_value());

        qa_drain::SerialScheduler scheduler;
        expect(scheduler.exchange(std::move(flow)).has_value());
        expect(qa_drain::runWithin(scheduler, std::chrono::duration_cast<std::chrono::milliseconds>(qa_drain::kRunBound))) << "the graph did not end";

        expect(eq(relay._nForwarded, qa_drain::kBurst)) << "the end of the stream cut the block short of the items already in its queue";
        expect(eq(sink._nReceived, qa_drain::kBurst)) << "items accepted upstream never reached the sink";
    };

    "a block that never takes its remainder does not hold the graph open"_test = [] {
        gr::Graph flow;
        auto&     source = flow.emplaceBlock<qa_drain::BurstSource>();
        auto&     relay  = flow.emplaceBlock<qa_drain::StuckRelay>();
        auto&     sink   = flow.emplaceBlock<qa_drain::CountingSink>();
        expect(flow.connect<"out", "in">(source, relay).has_value());
        expect(flow.connect<"out", "in">(relay, sink).has_value());

        qa_drain::SerialScheduler scheduler;
        gr::MsgPortIn             fromScheduler;
        expect(scheduler.msgOut.connect(fromScheduler).has_value());
        expect(scheduler.exchange(std::move(flow)).has_value());
        expect(qa_drain::runWithin(scheduler, std::chrono::duration_cast<std::chrono::milliseconds>(qa_drain::kRunBound))) << "a block making no progress held the graph open";

        expect(gt(relay._nCalls, 1UZ)) << "the block was not offered its remainder at all";
        expect(eq(sink._nReceived, 0UZ));
        expect(relay.state() == STOPPED);
        const std::vector<gr::Message> reports = qa_drain::takeDrainStallReports(fromScheduler, relay.unique_name);
        expect(eq(reports.size(), 1UZ)) << "the block's end was not reported once";
        if (reports.size() == 1UZ) {
            expect(eq(qa_drain::reportedValue(reports[0UZ], "min_samples"), relay.in.min_samples));
            expect(eq(qa_drain::reportedValue(reports[0UZ], "samples_left"), qa_drain::kBurst));
        }
    };

    "a restarted block counts its drain stalls from zero"_test = [] {
        gr::Graph flow;
        auto&     source = flow.emplaceBlock<qa_drain::BurstSource>();
        auto&     relay  = flow.emplaceBlock<qa_drain::StuckRelay>();
        auto&     sink   = flow.emplaceBlock<qa_drain::CountingSink>();
        expect(flow.connect<"out", "in">(source, relay).has_value());
        expect(flow.connect<"out", "in">(relay, sink).has_value());

        qa_drain::SerialScheduler scheduler;
        expect(scheduler.exchange(std::move(flow)).has_value());
        expect(qa_drain::runWithin(scheduler, std::chrono::duration_cast<std::chrono::milliseconds>(qa_drain::kRunBound))) << "the first run did not end";
        const std::size_t nCallsFirstRun = relay._nCalls;
        expect(relay.state() == STOPPED) << "the first run did not end the block";

        expect(qa_drain::runWithin(scheduler, std::chrono::duration_cast<std::chrono::milliseconds>(qa_drain::kRunBound))) << "the second run did not end";
        expect(eq(relay._nCalls - nCallsFirstRun, nCallsFirstRun)) << "the second run ended the block on the first run's stall count";
        expect(relay.state() == STOPPED);
    };

    "an input minimum published from processBulk governs the next call and ends the stream"_test = [] {
        gr::Graph flow;
        auto&     source = flow.emplaceBlock<qa_drain::BurstSource>();
        auto&     relay  = flow.emplaceBlock<qa_drain::WindowedRelay>();
        auto&     sink   = flow.emplaceBlock<qa_drain::CountingSink>();
        expect(flow.connect<"out", "in">(source, relay).has_value());
        expect(flow.connect<"out", "in">(relay, sink).has_value());

        qa_drain::SerialScheduler scheduler;
        expect(scheduler.exchange(std::move(flow)).has_value());
        expect(qa_drain::runWithin(scheduler, std::chrono::duration_cast<std::chrono::milliseconds>(qa_drain::kRunBound))) << "the block was offered a span it cannot use and the graph never ended";

        expect(eq(relay._nShortCalls, 0UZ)) << "the scheduler kept offering less than the published minimum";
        expect(eq(sink._nReceived, qa_drain::kBurst - qa_drain::kWindow + 1UZ)) << "one item per sample the window could be filled from";
        expect(relay._epilogueRan) << "a block whose minimum can no longer be met was not driven to its epilogue";
        expect(eq(relay._tail, qa_drain::kWindow - 1UZ)) << "the tail is what no window could be filled from";
    };

    "an asynchronous input's end leaves the block running while another input is owed items"_test = [] {
        gr::Graph flow;
        auto&     shortSource = flow.emplaceBlock<qa_drain::CountedSource>();
        auto&     longSource  = flow.emplaceBlock<qa_drain::CountedSource>();
        auto&     sink        = flow.emplaceBlock<qa_drain::AsyncPairSink>();

        shortSource._nItems     = qa_drain::kShortBranch;
        shortSource._maxPerCall = qa_drain::kShortBranch;
        longSource._nItems      = qa_drain::kLongBranch;
        longSource._maxPerCall  = qa_drain::kMaxPerCall;
        expect(flow.connect<"out", "first">(shortSource, sink).has_value());
        expect(flow.connect<"out", "second">(longSource, sink).has_value());

        qa_drain::SerialScheduler scheduler;
        expect(scheduler.exchange(std::move(flow)).has_value());
        expect(qa_drain::runWithin(scheduler, std::chrono::duration_cast<std::chrono::milliseconds>(qa_drain::kRunBound))) << "the graph did not end";

        expect(eq(sink._nFirst, qa_drain::kShortBranch));
        expect(eq(sink._nSecond, qa_drain::kLongBranch)) << "the block ended at the end of its short branch";
        expect(eq(sink._nFirst + sink._nSecond, qa_drain::kShortBranch + qa_drain::kLongBranch));
        expect(sink.state() == STOPPED);
    };

    "an asynchronous input's end leaves the block running while its synchronous input is owed samples"_test = [] {
        gr::Graph flow;
        auto&     shortSource = flow.emplaceBlock<qa_drain::CountedSource>();
        auto&     longSource  = flow.emplaceBlock<qa_drain::CountedSource>();
        auto&     sink        = flow.emplaceBlock<qa_drain::MixedPairSink>();

        shortSource._nItems     = qa_drain::kShortBranch;
        shortSource._maxPerCall = qa_drain::kShortBranch;
        longSource._nItems      = qa_drain::kLongBranch;
        longSource._maxPerCall  = qa_drain::kMaxPerCall;
        expect(flow.connect<"out", "aux">(shortSource, sink).has_value());
        expect(flow.connect<"out", "data">(longSource, sink).has_value());

        qa_drain::SerialScheduler scheduler;
        expect(scheduler.exchange(std::move(flow)).has_value());
        expect(qa_drain::runWithin(scheduler, std::chrono::duration_cast<std::chrono::milliseconds>(qa_drain::kRunBound))) << "the graph did not end";

        expect(eq(sink._nAux, qa_drain::kShortBranch));
        expect(eq(sink._nData, qa_drain::kLongBranch)) << "the block ended at the end of its asynchronous input";
        expect(sink.state() == STOPPED);
    };

    "a block with two synchronous inputs fed by branches of different latency receives every sample"_test = [] {
        gr::Graph flow;
        auto&     source = flow.emplaceBlock<qa_drain::CountedSource>();
        auto&     relay  = flow.emplaceBlock<qa_drain::OneSamplePerCall>();
        auto&     sink   = flow.emplaceBlock<qa_drain::SyncPairSink>();

        source._nItems     = qa_drain::kStreamItems;
        source._maxPerCall = qa_drain::kMaxPerCall;
        expect(flow.connect<"out", "first">(source, sink).has_value());
        expect(flow.connect<"out", "in">(source, relay).has_value());
        expect(flow.connect<"out", "second">(relay, sink).has_value());

        qa_drain::SerialScheduler scheduler;
        expect(scheduler.exchange(std::move(flow)).has_value());
        expect(qa_drain::runWithin(scheduler, std::chrono::duration_cast<std::chrono::milliseconds>(qa_drain::kRunBound))) << "the graph did not end";

        expect(gt(sink._nCallsFirstEndOnly, 0UZ)) << "the direct edge's end never reached the block ahead of the slow branch's";
        expect(eq(sink._nReceived, qa_drain::kStreamItems)) << "the block ended at the end of its direct edge";
        expect(eq(sink._nMismatched, 0UZ)) << "the two inputs did not advance together";
        expect(sink.state() == STOPPED);
    };

    "a synchronous input's end leaves the block running while its asynchronous input is owed items"_test = [] {
        gr::Graph flow;
        auto&     source = flow.emplaceBlock<qa_drain::CountedSource>();
        auto&     relay  = flow.emplaceBlock<qa_drain::OneSamplePerCall>();
        auto&     sink   = flow.emplaceBlock<qa_drain::MixedPairSink>();

        source._nItems     = qa_drain::kStreamItems;
        source._maxPerCall = qa_drain::kMaxPerCall;
        expect(flow.connect<"out", "data">(source, sink).has_value());
        expect(flow.connect<"out", "in">(source, relay).has_value());
        expect(flow.connect<"out", "aux">(relay, sink).has_value());

        qa_drain::SerialScheduler scheduler;
        expect(scheduler.exchange(std::move(flow)).has_value());
        expect(qa_drain::runWithin(scheduler, std::chrono::duration_cast<std::chrono::milliseconds>(qa_drain::kRunBound))) << "the graph did not end";

        expect(eq(sink._nData, qa_drain::kStreamItems));
        expect(eq(sink._nAux, qa_drain::kStreamItems)) << "the block ended at the end of its synchronous input";
        expect(gt(sink._nCallsAfterDataEnd, 0UZ)) << "the block did not run with its synchronous input at its end";
        expect(sink.state() == STOPPED);
    };

    "a block that requests its own stop at the end of one input ends there"_test = [] {
        gr::Graph flow;
        auto&     shortSource = flow.emplaceBlock<qa_drain::CountedSource>();
        auto&     longSource  = flow.emplaceBlock<qa_drain::CountedSource>();
        auto&     sink        = flow.emplaceBlock<qa_drain::FirstEndSink>();

        shortSource._nItems     = qa_drain::kShortBranch;
        shortSource._maxPerCall = qa_drain::kShortBranch;
        longSource._nItems      = qa_drain::kLongBranch;
        longSource._maxPerCall  = qa_drain::kMaxPerCall;
        expect(flow.connect<"out", "first">(shortSource, sink).has_value());
        expect(flow.connect<"out", "second">(longSource, sink).has_value());

        qa_drain::SerialScheduler scheduler;
        expect(scheduler.exchange(std::move(flow)).has_value());
        expect(qa_drain::runWithin(scheduler, std::chrono::duration_cast<std::chrono::milliseconds>(qa_drain::kRunBound))) << "the graph did not end after the block stopped";

        expect(eq(sink._nFirst, qa_drain::kShortBranch));
        expect(lt(sink._nSecond, qa_drain::kLongBranch)) << "the block ran past the end of its first input";
        expect(sink.state() == STOPPED);
        expect(longSource.state() == STOPPED) << "the source left without a reader did not stop";
    };

    "a block holding back the end of a finite stream ends within the bound and reports what it left"_test = [] {
        gr::Graph flow;
        auto&     source = flow.emplaceBlock<qa_drain::BurstSource>();
        auto&     relay  = flow.emplaceBlock<qa_drain::HoldBackRelay>();
        auto&     sink   = flow.emplaceBlock<qa_drain::CountingSink>();
        expect(flow.connect<"out", "in">(source, relay).has_value());
        expect(flow.connect<"out", "in">(relay, sink).has_value());

        qa_drain::SerialScheduler scheduler;
        gr::MsgPortIn             fromScheduler;
        expect(scheduler.msgOut.connect(fromScheduler).has_value());
        expect(scheduler.exchange(std::move(flow)).has_value());
        expect(qa_drain::runWithin(scheduler, std::chrono::duration_cast<std::chrono::milliseconds>(qa_drain::kRunBound))) << "a block holding back the end of its stream held the graph open";

        expect(eq(sink._nReceived, qa_drain::kBurst - qa_drain::kWindow + 1UZ)) << "one item per sample a window could be filled from";
        expect(relay.state() == STOPPED);
        const std::vector<gr::Message> reports = qa_drain::takeDrainStallReports(fromScheduler, relay.unique_name);
        expect(eq(reports.size(), 1UZ)) << "the block's end was not reported once";
        if (reports.size() == 1UZ) {
            expect(eq(qa_drain::reportedValue(reports[0UZ], "min_samples"), relay.in.min_samples));
            expect(eq(qa_drain::reportedValue(reports[0UZ], "samples_left"), qa_drain::kWindow - 1UZ));
        }
    };

    "a block waiting on a full output is not ended while its input holds samples"_test = [] {
        gr::Graph flow;
        auto&     source = flow.emplaceBlock<qa_drain::RingAndAHalfSource>();
        auto&     relay  = flow.emplaceBlock<qa_drain::Passthrough>();
        auto&     sink   = flow.emplaceBlock<qa_drain::LateSink>();

        sink._source = &source;
        expect(flow.connect<"out", "in">(source, relay).has_value());
        expect(flow.connect<"out", "in">(relay, sink).has_value());

        qa_drain::SerialScheduler scheduler;
        gr::MsgPortIn             fromScheduler;
        expect(scheduler.msgOut.connect(fromScheduler).has_value());
        expect(scheduler.exchange(std::move(flow)).has_value());
        expect(qa_drain::runWithin(scheduler, std::chrono::duration_cast<std::chrono::milliseconds>(qa_drain::kSlowConsumerBound))) << "the graph did not end";

        expect(eq(sink._nLargestDeclined, sink._ringSize)) << "the relay's output never filled while the sink declined";
        expect(gt(source._nItems, sink._ringSize)) << "the relay held no samples behind its full output";
        expect(gt(sink._nDeclinedAfterSourceEnded, qa_drain::kDrainStallCalls)) << "the relay's input end was not in view for long while the sink declined";
        expect(eq(sink._nReceived, source._nItems)) << "the relay ended while a slow consumer kept its output full";
        expect(qa_drain::takeDrainStallReports(fromScheduler, relay.unique_name).empty());
    };

    "a resampling block whose output has room for less than one chunk is not ended while its input holds samples"_test = [] {
        gr::Graph flow;
        auto&     source       = flow.emplaceBlock<qa_drain::RingAndAHalfSource>();
        auto&     interpolator = flow.emplaceBlock<qa_drain::Interpolator>();
        auto&     sink         = flow.emplaceBlock<qa_drain::LateSink>();

        sink._source         = &source;
        sink._nTakenWhenFull = 1UZ;
        expect(flow.connect<"out", "in">(source, interpolator).has_value());
        expect(flow.connect<"out", "in">(interpolator, sink).has_value());

        qa_drain::SerialScheduler scheduler;
        gr::MsgPortIn             fromScheduler;
        expect(scheduler.msgOut.connect(fromScheduler).has_value());
        expect(scheduler.exchange(std::move(flow)).has_value());
        expect(qa_drain::runWithin(scheduler, std::chrono::duration_cast<std::chrono::milliseconds>(qa_drain::kSlowConsumerBound))) << "the graph did not end";

        expect(sink._tookWhenFull) << "the interpolator's output never filled";
        expect(eq(sink._nLargestDeclined + sink._nTakenWhenFull, sink._ringSize)) << "the interpolator's output had room for a chunk while the sink declined";
        expect(gt(sink._nDeclinedAfterSourceEnded, qa_drain::kDrainStallCalls)) << "the interpolator's input end was not in view for long while the sink declined";
        expect(eq(sink._nReceived, source._nItems * qa_drain::kInterpolation)) << "the interpolator ended while its output had room for less than one chunk";
        expect(qa_drain::takeDrainStallReports(fromScheduler, interpolator.unique_name).empty());
    };

    "a block that returns INSUFFICIENT_OUTPUT_ITEMS is not ended while its input holds samples"_test = [] {
        gr::Graph flow;
        auto&     source = flow.emplaceBlock<qa_drain::BurstSource>();
        auto&     sink   = flow.emplaceBlock<qa_drain::DeviceSink>();
        expect(flow.connect<"out", "in">(source, sink).has_value());

        qa_drain::SerialScheduler scheduler;
        gr::MsgPortIn             fromScheduler;
        expect(scheduler.msgOut.connect(fromScheduler).has_value());
        expect(scheduler.exchange(std::move(flow)).has_value());
        expect(qa_drain::runWithin(scheduler, std::chrono::duration_cast<std::chrono::milliseconds>(qa_drain::kSlowConsumerBound))) << "the graph did not end";

        expect(gt(sink._nWaitsWithEndInView, qa_drain::kDrainStallCalls)) << "the sink's input end was not in view for long while it waited";
        expect(eq(sink._nReceived, qa_drain::kBurst)) << "the sink was ended while it waited on its device";
        expect(qa_drain::takeDrainStallReports(fromScheduler, sink.unique_name).empty());
    };

    "a block whose output is full at the end of its stream runs its epilogue once the output has room"_test = [] {
        gr::Graph flow;
        auto&     source    = flow.emplaceBlock<qa_drain::TailSource>();
        auto&     decimator = flow.emplaceBlock<qa_drain::TailDecimator>();
        auto&     sink      = flow.emplaceBlock<qa_drain::LateSink>();
        sink._nLateCalls    = qa_drain::kDrainStallCalls / 2UZ; // fewer calls than the epilogue waits on outputs that do not move
        expect(flow.connect<"out", "in">(source, decimator).has_value());
        expect(flow.connect<"out", "in">(decimator, sink).has_value());

        qa_drain::SerialScheduler scheduler;
        expect(scheduler.exchange(std::move(flow)).has_value());
        expect(qa_drain::runWithin(scheduler, std::chrono::duration_cast<std::chrono::milliseconds>(qa_drain::kSlowConsumerBound))) << "the graph did not end";

        const std::size_t nExpected = (source._nItems + qa_drain::kDecimation - 1UZ) / qa_drain::kDecimation;
        expect(eq(sink._nLargestDeclined, sink._ringSize)) << "the decimator's output never filled while the sink declined";
        expect(eq(decimator._epilogueSpan, (qa_drain::kTail + qa_drain::kDecimation - 1UZ) / qa_drain::kDecimation)) << "the epilogue was not handed the span it asked for";
        expect(eq(sink._nReceived, nExpected)) << "the sample the epilogue makes from the tail was lost";
    };

    "a block stopped with less room in its output than its tail needs is handed that room and writes within it"_test = [] {
        gr::PortOut<int>           upstream;
        gr::PortIn<int>            downstream;
        qa_drain::TailInterpolator block;
        const std::size_t          ring  = qa_drain::connectBetween(block, upstream, downstream);
        const std::size_t          nTail = ring / 2UZ;
        const std::size_t          room  = ring / 4UZ;
        const std::size_t          held  = ring - room;
        expect(fatal(lt(room, nTail * qa_drain::kInterpolation))) << "the room holds the whole tail";
        qa_drain::publishSamples(upstream, nTail);
        qa_drain::publishSamples(block.out, held);

        std::ignore                   = block.settings().setStaged({{"stop_now", true}});
        const gr::work::Result result = block.work();

        expect(result.status == gr::work::Status::DONE);
        expect(block.state() == STOPPED);
        expect(eq(block._epilogueTail, nTail));
        expect(eq(block._epilogueSpan, room)) << "the epilogue was not handed the room its output has";
        expect(eq(block._epilogueWritten, room - room % qa_drain::kInterpolation));
        expect(eq(downstream.streamReader().available(), held + block._epilogueWritten)) << "the output does not hold what the epilogue wrote";
    };

    "a block that writes nothing into a short span on a stop request ends its stream"_test = [] {
        gr::PortOut<int>           upstream;
        gr::PortIn<int>            downstream;
        qa_drain::TailInterpolator block;
        block._wholeTailOnly    = true;
        const std::size_t ring  = qa_drain::connectBetween(block, upstream, downstream);
        const std::size_t nTail = ring / 2UZ;
        const std::size_t room  = ring / 4UZ;
        const std::size_t held  = ring - room;
        expect(fatal(lt(room, nTail * qa_drain::kInterpolation))) << "the room holds the whole tail";
        qa_drain::publishSamples(upstream, nTail);
        qa_drain::publishSamples(block.out, held);

        std::ignore                   = block.settings().setStaged({{"stop_now", true}});
        const gr::work::Result result = block.work();

        expect(result.status == gr::work::Status::DONE);
        expect(block.state() == STOPPED);
        expect(eq(block._epilogueSpan, room)) << "the epilogue was not handed the room its output has";
        expect(eq(block._epilogueWritten, 0UZ));
        expect(eq(downstream.streamReader().available(), held)) << "the output holds samples the epilogue did not write";
        expect(qa_drain::endOfStreamIndex(downstream) == std::optional<std::size_t>(held)) << "the end of the stream does not follow the last sample written";
    };

    "a block whose tail needs more than its output ring is handed the whole ring"_test = [] {
        gr::PortOut<int>           upstream;
        gr::PortIn<int>            downstream;
        qa_drain::TailInterpolator block;
        const std::size_t          ring  = qa_drain::connectBetween(block, upstream, downstream);
        const std::size_t          nTail = ring - ring / 4UZ;
        expect(fatal(lt(nTail, upstream.bufferSize()))) << "the input ring does not hold the tail";
        expect(fatal(gt(nTail * qa_drain::kInterpolation, ring))) << "the output ring holds the whole tail";
        qa_drain::publishSamples(upstream, nTail);
        upstream.publishEoSTag(gr::property_map{{gr::tag::END_OF_STREAM.key(), true}});
        block.in.min_samples = nTail + 1UZ; // the stream ends before the block can take its tail

        const gr::work::Result result = block.work();

        expect(result.status == gr::work::Status::DONE) << "the block waits for room its output can never have";
        expect(eq(block._epilogueTail, nTail));
        expect(eq(block._epilogueSpan, ring)) << "the epilogue was not handed the whole ring";
        expect(eq(downstream.streamReader().available(), ring - ring % qa_drain::kInterpolation)) << "the output does not hold what the epilogue wrote";
    };

    "a block whose epilogue waits on a downstream block holding back a partial chunk ends its stream"_test = [] {
        gr::PortOut<int>           upstream;
        qa_drain::TailInterpolator interpolator;
        qa_drain::TailDecimator    decimator;
        gr::PortIn<int>            downstream;
        expect(fatal(upstream.connect(interpolator.in).has_value()));
        expect(fatal(interpolator.out.connect(decimator.in).has_value()));
        expect(fatal(decimator.out.connect(downstream).has_value()));
        interpolator.init(std::make_shared<gr::Sequence>());
        decimator.init(std::make_shared<gr::Sequence>());
        const std::size_t ring = interpolator.out.bufferSize();

        // one sample makes kInterpolation, fewer than the decimator's chunk: it holds them until its stream ends
        qa_drain::publishSamples(upstream, 1UZ);
        std::ignore                = interpolator.work();
        std::ignore                = decimator.work();
        const std::size_t heldBack = decimator.in.streamReader().available();
        expect(fatal(eq(heldBack, std::size_t{qa_drain::kInterpolation}))) << "the decimator does not hold back the partial chunk";

        const std::size_t nTail = ring - ring / 4UZ; // needs more than the ring, so the request is the whole ring
        qa_drain::publishSamples(upstream, nTail);
        upstream.publishEoSTag(gr::property_map{{gr::tag::END_OF_STREAM.key(), true}});
        interpolator.in.min_samples = nTail + 1UZ;

        std::size_t nCalls = 0UZ;
        bool        ended  = false;
        while (!ended && nCalls < 4UZ * qa_drain::kDrainStallCalls) {
            ended       = interpolator.work().status == gr::work::Status::DONE;
            std::ignore = decimator.work();
            nCalls++;
        }
        expect(ended) << "the epilogue waits for room the downstream block never makes";
        expect(gt(nCalls, qa_drain::kDrainStallCalls)) << "the epilogue did not wait for the outputs to drain";
        expect(eq(interpolator._epilogueSpan, ring - heldBack)) << "the epilogue was not handed the room the output has";
        std::ignore = decimator.work();
        expect(decimator.state() == STOPPED) << "the end of the stream did not reach the downstream block";
    };

    "a block whose epilogue waits for room ends its stream within the bound under singleThreadedBlocking"_test = [] { qa_drain::expectEpilogueWaitEndsUnderScheduler<gr::scheduler::Simple<gr::scheduler::ExecutionPolicy::singleThreadedBlocking>>(); };

    "a block whose epilogue waits for room ends its stream within the bound under multiThreadedBlocking"_test = [] { qa_drain::expectEpilogueWaitEndsUnderScheduler<gr::scheduler::Simple<gr::scheduler::ExecutionPolicy::multiThreadedBlocking>>(); };

    "a drain stall held while the epilogue waits for room is reported once, and the epilogue runs once room appears"_test = [] {
        gr::PortOut<int>                upstream;
        qa_drain::DecliningInterpolator block;
        gr::PortIn<int>                 downstream;
        gr::MsgPortIn                   fromBlock;
        expect(fatal(upstream.connect(block.in).has_value()));
        expect(fatal(block.out.connect(downstream).has_value()));
        expect(block.msgOut.connect(fromBlock).has_value());
        block.init(std::make_shared<gr::Sequence>());
        const std::size_t ring  = block.out.bufferSize();
        const std::size_t nTail = 16UZ;
        const std::size_t room  = nTail; // less than the nTail * kInterpolation the epilogue asks for
        qa_drain::publishSamples(block.out, ring - room);
        qa_drain::publishSamples(upstream, nTail);
        upstream.publishEoSTag(gr::property_map{{gr::tag::END_OF_STREAM.key(), true}});

        // the stall comes after kDrainStallCalls calls. The calls after it wait, fewer than the epilogue's own limit.
        std::size_t nDone    = 0UZ;
        std::size_t nReports = 0UZ;
        for (std::size_t i = 0UZ; i < qa_drain::kDrainStallCalls + qa_drain::kDrainStallCalls / 2UZ; ++i) {
            nDone += block.work().status == gr::work::Status::DONE ? 1UZ : 0UZ;
            nReports += qa_drain::takeDrainStallReports(fromBlock, block.unique_name).size();
        }
        expect(eq(nDone, 0UZ)) << "the epilogue ran without room for its request";
        expect(eq(nReports, 1UZ)) << "the stall was not reported once while the epilogue waited";

        {
            auto taken  = downstream.streamReader().get();
            std::ignore = taken.consume(taken.size());
        }
        expect(block.work().status == gr::work::Status::DONE) << "the epilogue did not run on the first call with room";
        expect(eq(block._epilogueSpan, nTail * qa_drain::kInterpolation));
        expect(eq(qa_drain::takeDrainStallReports(fromBlock, block.unique_name).size(), 0UZ)) << "the stall was reported again";
    };

    "a block with two outputs waits for room in the fuller one, and on a stop request takes its room on both"_test = [] {
        gr::PortOut<int>        upstream;
        gr::PortIn<int>         firstDownstream;
        gr::PortIn<int>         secondDownstream;
        qa_drain::TwoOutputTail block;
        expect(fatal(upstream.connect(block.in).has_value()));
        expect(fatal(block.first.connect(firstDownstream).has_value()));
        expect(fatal(block.second.connect(secondDownstream).has_value()));
        block.init(std::make_shared<gr::Sequence>());
        const std::size_t ring  = block.first.bufferSize();
        const std::size_t nTail = ring / 2UZ;
        const std::size_t room  = ring / 4UZ;
        expect(fatal(lt(room, nTail)));
        expect(fatal(ge(block.second.bufferSize(), nTail)));
        qa_drain::publishSamples(block.first, ring - room);
        qa_drain::publishSamples(upstream, nTail);
        upstream.publishEoSTag(gr::property_map{{gr::tag::END_OF_STREAM.key(), true}});
        block.in.min_samples = nTail + 1UZ; // the stream ends before the block can take its tail

        const gr::work::Result waited = block.work();
        expect(waited.status == gr::work::Status::INSUFFICIENT_OUTPUT_ITEMS) << "the epilogue did not wait for room in the fuller output";
        expect(!block._epilogueRan);
        expect(eq(block.in.streamReader().available(), nTail)) << "the waiting epilogue consumed its tail";

        std::ignore                   = block.settings().setStaged({{"stop_now", true}});
        const gr::work::Result result = block.work();
        expect(result.status == gr::work::Status::DONE);
        expect(eq(block._firstSpan, room)) << "the fuller output's span is not its room";
        expect(eq(block._secondSpan, room)) << "the emptier output's span is not the fuller one's room";
        expect(eq(secondDownstream.streamReader().available(), room));
    };

    "a block without an epilogue reports the tail its input's min_samples drops at the end of the stream"_test = [] {
        constexpr std::size_t kItems  = 70UZ;
        constexpr std::size_t kHeldTo = 16UZ;
        for (const std::size_t minSamples : {kHeldTo, 1UZ}) {
            gr::Graph flow;
            auto&     source    = flow.emplaceBlock<qa_drain::CountedSource>();
            auto&     copy      = flow.emplaceBlock<qa_drain::Passthrough>();
            auto&     sink      = flow.emplaceBlock<qa_drain::CountingSink>();
            source._nItems      = kItems;
            source._maxPerCall  = kItems;
            copy.in.min_samples = minSamples;
            copy.in.max_samples = kHeldTo;
            expect(flow.connect<"out", "in">(source, copy).has_value());
            expect(flow.connect<"out", "in">(copy, sink).has_value());

            qa_drain::SerialScheduler      scheduler;
            gr::MsgPortIn                  fromScheduler;
            const std::vector<gr::Message> reports  = qa_drain::runAndTakeDroppedTailReports(scheduler, fromScheduler, std::move(flow), copy.unique_name);
            const std::size_t              nDropped = minSamples > 1UZ ? kItems % kHeldTo : 0UZ;
            expect(eq(sink._nReceived, kItems - nDropped)) << std::format("min_samples {}: the sink does not receive the samples before the tail", minSamples);
            expect(eq(reports.size(), nDropped > 0UZ ? 1UZ : 0UZ)) << std::format("min_samples {}: the dropped tail is not reported once", minSamples);
            if (reports.size() == 1UZ) {
                expect(eq(qa_drain::reportedValue(reports[0UZ], "samples_dropped"), nDropped));
                expect(qa_drain::reportedText(reports[0UZ], "port") == copy.in.metaInfo.name.value) << "the report does not name the port";
            }
        }
    };

    "a decimator without an epilogue reports the partial chunk its input_chunk_size drops at the end of the stream"_test = [] {
        constexpr std::size_t kChunks = 16UZ;
        const std::size_t     nItems  = kChunks * qa_drain::kDecimation + qa_drain::kTail;
        gr::Graph             flow;
        auto&                 source    = flow.emplaceBlock<qa_drain::CountedSource>();
        auto&                 decimator = flow.emplaceBlock<qa_drain::ChunkDecimator>();
        auto&                 sink      = flow.emplaceBlock<qa_drain::CountingSink>();
        source._nItems                  = nItems;
        source._maxPerCall              = nItems;
        decimator.in.min_samples        = 1UZ;
        expect(flow.connect<"out", "in">(source, decimator).has_value());
        expect(flow.connect<"out", "in">(decimator, sink).has_value());

        qa_drain::SerialScheduler      scheduler;
        gr::MsgPortIn                  fromScheduler;
        const std::vector<gr::Message> reports = qa_drain::runAndTakeDroppedTailReports(scheduler, fromScheduler, std::move(flow), decimator.unique_name);
        const std::size_t              chunk   = static_cast<std::size_t>(decimator.input_chunk_size);
        expect(eq(sink._nReceived, nItems / chunk)) << "the sink does not receive one sample per whole chunk";
        expect(fatal(eq(reports.size(), 1UZ))) << "the partial chunk is not reported once";
        expect(eq(qa_drain::reportedValue(reports[0UZ], "samples_dropped"), nItems % chunk));
        expect(qa_drain::reportedText(reports[0UZ], "port") == decimator.in.metaInfo.name.value) << "the report does not name the port";
    };

    "a block with two synchronous inputs reports the tail that the other input's larger min_samples drops"_test = [] {
        constexpr std::size_t kItems     = 70UZ;
        constexpr std::size_t kLargerMin = 16UZ;
        gr::Graph             flow;
        auto&                 firstSource  = flow.emplaceBlock<qa_drain::CountedSource>();
        auto&                 secondSource = flow.emplaceBlock<qa_drain::CountedSource>();
        auto&                 pair         = flow.emplaceBlock<qa_drain::SyncPairSink>();
        for (qa_drain::CountedSource* source : {&firstSource, &secondSource}) {
            source->_nItems     = kItems;
            source->_maxPerCall = kItems;
        }
        pair.first.min_samples  = 1UZ;
        pair.second.min_samples = kLargerMin;
        pair.second.max_samples = kLargerMin; // each call takes kLargerMin samples from both inputs
        expect(flow.connect<"out", "first">(firstSource, pair).has_value());
        expect(flow.connect<"out", "second">(secondSource, pair).has_value());

        qa_drain::SerialScheduler      scheduler;
        gr::MsgPortIn                  fromScheduler;
        const std::vector<gr::Message> reports  = qa_drain::runAndTakeDroppedTailReports(scheduler, fromScheduler, std::move(flow), pair.unique_name);
        const std::size_t              nDropped = kItems % kLargerMin;
        expect(eq(pair._nReceived, kItems - nDropped)) << "the block does not receive the samples before the tail";
        expect(fatal(eq(reports.size(), 2UZ))) << "the tail of each input is not reported once";
        expect(qa_drain::reportedText(reports[0UZ], "port") == pair.first.metaInfo.name.value) << "the first report does not name the first input";
        expect(qa_drain::reportedText(reports[1UZ], "port") == pair.second.metaInfo.name.value) << "the second report does not name the second input";
        for (const gr::Message& report : reports) {
            expect(eq(qa_drain::reportedValue(report, "samples_dropped"), nDropped));
        }
    };

    "a block whose output min_samples ends its stream reports the tail it was never handed"_test = [] {
        constexpr std::size_t kItems  = 70UZ;
        constexpr std::size_t kHeldTo = 16UZ;
        gr::Graph             flow;
        auto&                 source = flow.emplaceBlock<qa_drain::CountedSource>();
        auto&                 copy   = flow.emplaceBlock<qa_drain::Passthrough>();
        auto&                 sink   = flow.emplaceBlock<qa_drain::CountingSink>();
        source._nItems               = kItems;
        source._maxPerCall           = kItems;
        copy.in.max_samples          = kHeldTo;
        copy.out.min_samples         = kHeldTo;
        expect(flow.connect<"out", "in">(source, copy).has_value());
        expect(flow.connect<"out", "in">(copy, sink).has_value());

        qa_drain::SerialScheduler      scheduler;
        gr::MsgPortIn                  fromScheduler;
        const std::vector<gr::Message> reports  = qa_drain::runAndTakeDroppedTailReports(scheduler, fromScheduler, std::move(flow), copy.unique_name);
        const std::size_t              nDropped = kItems % kHeldTo;
        expect(eq(sink._nReceived, kItems - nDropped)) << "the sink does not receive the whole output chunks";
        expect(fatal(eq(reports.size(), 1UZ))) << "the tail is not reported once";
        expect(eq(qa_drain::reportedValue(reports[0UZ], "samples_dropped"), nDropped));
        expect(qa_drain::reportedText(reports[0UZ], "port") == copy.in.metaInfo.name.value) << "the report does not name the port";
    };

    "a block with two synchronous inputs of unequal length reports what each input holds at the end"_test = [] {
        constexpr std::size_t kHeldTo = 16UZ;
        for (const auto& [firstItems, secondItems] : {std::pair{70UZ, 60UZ}, std::pair{90UZ, 64UZ}}) {
            gr::Graph flow;
            auto&     firstSource    = flow.emplaceBlock<qa_drain::CountedSource>();
            auto&     secondSource   = flow.emplaceBlock<qa_drain::CountedSource>();
            auto&     pair           = flow.emplaceBlock<qa_drain::SyncPairSink>();
            firstSource._nItems      = firstItems;
            firstSource._maxPerCall  = firstItems;
            secondSource._nItems     = secondItems;
            secondSource._maxPerCall = secondItems;
            pair.first.min_samples   = kHeldTo;
            pair.second.min_samples  = kHeldTo;
            pair.second.max_samples  = kHeldTo;
            expect(flow.connect<"out", "first">(firstSource, pair).has_value());
            expect(flow.connect<"out", "second">(secondSource, pair).has_value());

            qa_drain::SerialScheduler      scheduler;
            gr::MsgPortIn                  fromScheduler;
            const std::vector<gr::Message> reports   = qa_drain::runAndTakeDroppedTailReports(scheduler, fromScheduler, std::move(flow), pair.unique_name);
            const std::size_t              nReceived = std::min(firstItems, secondItems) / kHeldTo * kHeldTo;
            expect(eq(pair._nReceived, nReceived)) << std::format("{} and {}: the block does not receive the whole chunks of the shorter input", firstItems, secondItems);
            std::string expected; // "<port> <samples_dropped>; " for each input that holds samples, in port order
            if (firstItems > nReceived) {
                expected += std::format("{} {}; ", pair.first.metaInfo.name.value, firstItems - nReceived);
            }
            if (secondItems > nReceived) {
                expected += std::format("{} {}; ", pair.second.metaInfo.name.value, secondItems - nReceived);
            }
            std::string reported;
            for (const gr::Message& report : reports) {
                reported += std::format("{} {}; ", qa_drain::reportedText(report, "port"), qa_drain::reportedValue(report, "samples_dropped"));
            }
            expect(eq(reported, expected)) << std::format("{} and {}: the reports are not one per input that holds samples, with what it holds", firstItems, secondItems);
        }
    };

    "a block whose stream ends while another input has no end in view reports what that input holds"_test = [] {
        constexpr std::size_t  kFirstItems  = 5UZ;
        constexpr std::size_t  kSecondItems = 9UZ;
        gr::PortOut<int>       firstUpstream;
        gr::PortOut<int>       secondUpstream;
        qa_drain::SyncPairSink block;
        gr::MsgPortIn          fromBlock;
        expect(fatal(firstUpstream.connect(block.first).has_value()));
        expect(fatal(secondUpstream.connect(block.second).has_value()));
        expect(block.msgOut.connect(fromBlock).has_value());
        block.init(std::make_shared<gr::Sequence>());
        qa_drain::publishSamples(firstUpstream, kFirstItems);
        firstUpstream.publishEoSTag(gr::property_map{{gr::tag::END_OF_STREAM.key(), true}});
        qa_drain::publishSamples(secondUpstream, kSecondItems);

        gr::work::Result result;
        for (std::size_t call = 0UZ; call < 4UZ && result.status != gr::work::Status::DONE; ++call) {
            result = block.work();
        }
        expect(result.status == gr::work::Status::DONE) << "the end of the first input does not end the block";
        expect(eq(block._nReceived, kFirstItems));
        const std::vector<gr::Message> reports = qa_drain::takeDroppedTailReports(fromBlock, block.unique_name);
        expect(fatal(eq(reports.size(), 1UZ))) << "the input without an end is not reported once";
        expect(qa_drain::reportedText(reports[0UZ], "port") == block.second.metaInfo.name.value) << "the report does not name the input without an end";
        expect(eq(qa_drain::reportedValue(reports[0UZ], "samples_dropped"), kSecondItems - kFirstItems));
    };

    "a block with overlapping chunks sends no report for the tail its last call was handed"_test = [] {
        constexpr std::size_t kItems = 16UZ;
        gr::Graph             flow;
        auto&                 source    = flow.emplaceBlock<qa_drain::CountedSource>();
        auto&                 decimator = flow.emplaceBlock<qa_drain::StridedDecimator>();
        auto&                 sink      = flow.emplaceBlock<qa_drain::CountingSink>();
        source._nItems                  = kItems;
        source._maxPerCall              = kItems;
        expect(flow.connect<"out", "in">(source, decimator).has_value());
        expect(flow.connect<"out", "in">(decimator, sink).has_value());

        qa_drain::SerialScheduler      scheduler;
        gr::MsgPortIn                  fromScheduler;
        const std::vector<gr::Message> reports = qa_drain::runAndTakeDroppedTailReports(scheduler, fromScheduler, std::move(flow), decimator.unique_name);
        const std::size_t              chunk   = static_cast<std::size_t>(decimator.input_chunk_size);
        expect(eq(decimator._highestHanded, static_cast<int>(kItems) - 1)) << "the last call was not handed the last sample";
        expect(eq(sink._nReceived, kItems - chunk + 1UZ)) << "the sink does not receive one sample per chunk position";
        expect(eq(reports.size(), 0UZ)) << "samples the last call was handed were reported as dropped";
    };

    "a block with a port collection names each input of the collection in its report"_test = [] {
        constexpr std::size_t kItems  = 70UZ;
        constexpr std::size_t kHeldTo = 16UZ;
        gr::Graph             flow;
        auto&                 firstSource  = flow.emplaceBlock<qa_drain::CountedSource>();
        auto&                 secondSource = flow.emplaceBlock<qa_drain::CountedSource>();
        auto&                 pair         = flow.emplaceBlock<qa_drain::CollectionPairSink>();
        for (qa_drain::CountedSource* source : {&firstSource, &secondSource}) {
            source->_nItems     = kItems;
            source->_maxPerCall = kItems;
        }
        pair.in[1UZ].min_samples = kHeldTo;
        pair.in[1UZ].max_samples = kHeldTo;
        expect(fatal(flow.connect(firstSource, gr::PortDefinition("out"), pair, gr::PortDefinition("in#0")).has_value()));
        expect(fatal(flow.connect(secondSource, gr::PortDefinition("out"), pair, gr::PortDefinition("in#1")).has_value()));

        qa_drain::SerialScheduler      scheduler;
        gr::MsgPortIn                  fromScheduler;
        const std::vector<gr::Message> reports  = qa_drain::runAndTakeDroppedTailReports(scheduler, fromScheduler, std::move(flow), pair.unique_name);
        const std::size_t              nDropped = kItems % kHeldTo;
        expect(eq(pair._nReceived, kItems - nDropped)) << "the block does not receive the samples before the tail";
        expect(fatal(eq(reports.size(), 2UZ))) << "the tail of each input is not reported once";
        for (std::size_t index = 0UZ; index < reports.size(); ++index) {
            expect(qa_drain::reportedText(reports[index], "port") == std::format("in#{}", index)) << std::format("report {} does not name the input as connect() takes it", index);
            expect(eq(qa_drain::reportedValue(reports[index], "samples_dropped"), nDropped));
        }
    };

    "an asynchronous input that ends with fewer items than its min_samples reports them"_test = [] {
        constexpr std::size_t kItems = 1UZ;
        for (const std::size_t minSamples : {kItems + 1UZ, kItems}) {
            gr::PortOut<int>            upstream;
            qa_drain::AsyncCountingSink block;
            gr::MsgPortIn               fromBlock;
            expect(fatal(upstream.connect(block.in).has_value()));
            expect(block.msgOut.connect(fromBlock).has_value());
            block.in.min_samples = minSamples;
            block.init(std::make_shared<gr::Sequence>());
            qa_drain::publishSamples(upstream, kItems);
            upstream.publishEoSTag(gr::property_map{{gr::tag::END_OF_STREAM.key(), true}});

            gr::work::Result result;
            for (std::size_t call = 0UZ; call < 4UZ && result.status != gr::work::Status::DONE; ++call) {
                result = block.work();
            }
            expect(result.status == gr::work::Status::DONE) << std::format("min_samples {}: the end of the input does not end the block", minSamples);
            const std::size_t nDropped = kItems < minSamples ? kItems : 0UZ;
            expect(eq(block._nReceived, kItems - nDropped)) << std::format("min_samples {}: the block does not receive the items it can be handed", minSamples);
            const std::vector<gr::Message> reports = qa_drain::takeDroppedTailReports(fromBlock, block.unique_name);
            expect(eq(reports.size(), nDropped > 0UZ ? 1UZ : 0UZ)) << std::format("min_samples {}: the items left are not reported once", minSamples);
            if (reports.size() == 1UZ) {
                expect(eq(qa_drain::reportedValue(reports[0UZ], "samples_dropped"), nDropped));
                expect(qa_drain::reportedText(reports[0UZ], "port") == block.in.metaInfo.name.value) << "the report does not name the port";
            }
        }
    };

    "a block handed fewer samples than an earlier call was reports only the samples no call was handed"_test = [] {
        constexpr std::size_t kItems = 10UZ;
        for (const std::size_t nLate : {0UZ, 4UZ}) {
            gr::PortOut<int>          upstream;
            qa_drain::PartialConsumer block;
            gr::MsgPortIn             fromBlock;
            expect(fatal(upstream.connect(block.in).has_value()));
            expect(block.msgOut.connect(fromBlock).has_value());
            block._defaultMax = block.in.max_samples;
            block._lowerMax   = true;
            // after two calls the input holds kItems + nLate less two consumed chunks, which the raised minimum exceeds
            const std::size_t nHeldAfterSecond = kItems + nLate - 2UZ * qa_drain::PartialConsumer::kConsumed;
            block._minAfterSecondCall          = nHeldAfterSecond + 2UZ;
            block.init(std::make_shared<gr::Sequence>());
            qa_drain::publishSamples(upstream, kItems);

            gr::work::Result result = block.work();
            qa_drain::publishSamples(upstream, nLate);
            upstream.publishEoSTag(gr::property_map{{gr::tag::END_OF_STREAM.key(), true}});
            for (std::size_t call = 1UZ; call < 4UZ && result.status != gr::work::Status::DONE; ++call) {
                result = block.work();
            }
            expect(result.status == gr::work::Status::DONE) << std::format("{} late samples: the end of the input does not end the block", nLate);
            expect(fatal(eq(block._handed.size(), 2UZ))) << std::format("{} late samples: the block is not handed two spans", nLate);
            expect(eq(block._handed[0UZ], kItems));
            expect(eq(block._handed[1UZ], qa_drain::PartialConsumer::kLoweredMax)) << "the second call is not handed the lowered maximum";

            // the first call was handed kItems samples and the two calls consumed two chunks: the samples handed past
            // the read position are all but the ones the late publication added
            const std::vector<gr::Message> reports = qa_drain::takeDroppedTailReports(fromBlock, block.unique_name);
            expect(eq(reports.size(), nLate > 0UZ ? 1UZ : 0UZ)) << std::format("{} late samples: only the samples no call was handed are reported", nLate);
            if (reports.size() == 1UZ) {
                expect(eq(qa_drain::reportedValue(reports[0UZ], "samples_dropped"), nLate));
            }
        }
    };

    "a block whose stride skips past samples it was handed reports only the samples no call was handed"_test = [] {
        constexpr std::size_t kItems = 10UZ;
        for (const std::size_t nLate : {0UZ, 4UZ}) {
            gr::PortOut<int>     upstream;
            qa_drain::StrideSink block;
            gr::MsgPortIn        fromBlock;
            expect(fatal(upstream.connect(block.in).has_value()));
            expect(block.msgOut.connect(fromBlock).has_value());
            block._defaultMax = block.in.max_samples;
            // no call after the second is handed a span: the raised minimum exceeds everything the input can hold
            block._minAfterSecondCall = kItems + nLate + 1UZ;
            block.init(std::make_shared<gr::Sequence>());
            qa_drain::publishSamples(upstream, kItems);

            gr::work::Result result = block.work();
            qa_drain::publishSamples(upstream, nLate);
            upstream.publishEoSTag(gr::property_map{{gr::tag::END_OF_STREAM.key(), true}});
            for (std::size_t call = 1UZ; call < 4UZ && result.status != gr::work::Status::DONE; ++call) {
                result = block.work();
            }
            expect(result.status == gr::work::Status::DONE) << std::format("{} late samples: the end of the input does not end the block", nLate);
            expect(fatal(eq(block._handed.size(), 2UZ))) << std::format("{} late samples: the block is not handed two spans", nLate);
            expect(eq(block._handed[0UZ], kItems));
            expect(eq(block._handed[1UZ], qa_drain::StrideSink::kLoweredMax)) << "the second call is not handed the lowered maximum";

            // the calls were handed the first kItems samples, whatever the stride skipped over: the late ones are the samples no call was handed
            const std::vector<gr::Message> reports = qa_drain::takeDroppedTailReports(fromBlock, block.unique_name);
            expect(eq(reports.size(), nLate > 0UZ ? 1UZ : 0UZ)) << std::format("{} late samples: only the samples no call was handed are reported", nLate);
            if (reports.size() == 1UZ) {
                expect(eq(qa_drain::reportedValue(reports[0UZ], "samples_dropped"), nLate)) << "the samples the stride skipped over are reported";
            }
        }
    };

    "an asynchronous input that consumes one of two items is reported nothing for the other at its end"_test = [] {
        constexpr std::size_t      kItems = 2UZ;
        gr::PortOut<int>           upstream;
        qa_drain::AsyncPartialSink block;
        gr::MsgPortIn              fromBlock;
        expect(fatal(upstream.connect(block.in).has_value()));
        expect(block.msgOut.connect(fromBlock).has_value());
        block.in.min_samples = kItems;
        block.init(std::make_shared<gr::Sequence>());
        qa_drain::publishSamples(upstream, kItems);
        upstream.publishEoSTag(gr::property_map{{gr::tag::END_OF_STREAM.key(), true}});

        gr::work::Result result;
        for (std::size_t call = 0UZ; call < 4UZ && result.status != gr::work::Status::DONE; ++call) {
            result = block.work();
        }
        expect(result.status == gr::work::Status::DONE) << "the end of the input does not end the block";
        expect(eq(block._nHanded, kItems)) << "the block is not handed both items";
        expect(eq(block._nConsumed, 1UZ));
        expect(eq(qa_drain::takeDroppedTailReports(fromBlock, block.unique_name).size(), 0UZ)) << "the item the call was handed and left is reported";
    };

    "a block restarted after a stop counts the samples handed past its read position from zero"_test = [] {
        constexpr std::size_t     kItems     = 10UZ;
        constexpr std::size_t     kMoreItems = 3UZ;
        gr::PortOut<int>          upstream;
        qa_drain::PartialConsumer block;
        gr::MsgPortIn             fromBlock;
        expect(fatal(upstream.connect(block.in).has_value()));
        expect(block.msgOut.connect(fromBlock).has_value());
        block.init(std::make_shared<gr::Sequence>());
        expect(block.changeStateTo(RUNNING).has_value());
        qa_drain::publishSamples(upstream, kItems);

        // the first run's call is handed kItems samples and consumes kConsumed, which leaves the rest past the read position
        expect(block.work().status != gr::work::Status::DONE);
        expect(fatal(eq(block._handed.size(), 1UZ)));
        expect(eq(block._handed[0UZ], kItems));
        block.requestStop();
        expect(block.work().status == gr::work::Status::DONE);
        expect(block.state() == STOPPED);
        expect(eq(qa_drain::takeDroppedTailReports(fromBlock, block.unique_name).size(), 0UZ)) << "a stop request is reported";

        // a stopped block lets go of its input: the second run is fed through a new connection and needs one sample
        // more than the kMoreItems it is fed
        expect(block.changeStateTo(INITIALISED).has_value());
        expect(block.changeStateTo(RUNNING).has_value());
        expect(fatal(upstream.connect(block.in).has_value()));
        block.in.min_samples = kMoreItems + 1UZ;
        qa_drain::publishSamples(upstream, kMoreItems);
        upstream.publishEoSTag(gr::property_map{{gr::tag::END_OF_STREAM.key(), true}});
        gr::work::Result result;
        for (std::size_t call = 0UZ; call < 4UZ && result.status != gr::work::Status::DONE; ++call) {
            result = block.work();
        }
        expect(result.status == gr::work::Status::DONE) << "the end of the input does not end the restarted block";
        expect(eq(block._handed.size(), 1UZ)) << "the restarted block is handed a span before its end";
        const std::vector<gr::Message> reports = qa_drain::takeDroppedTailReports(fromBlock, block.unique_name);
        expect(fatal(eq(reports.size(), 1UZ))) << "the restarted block does not report what its input holds";
        expect(eq(qa_drain::reportedValue(reports[0UZ], "samples_dropped"), kMoreItems)) << "the restarted block subtracts samples its first run was handed";
    };

    "a block with an epilogue is handed its partial chunk and sends no dropped-tail report"_test = [] {
        constexpr std::size_t kChunks = 16UZ;
        const std::size_t     nItems  = kChunks * qa_drain::kDecimation + qa_drain::kTail;
        gr::Graph             flow;
        auto&                 source    = flow.emplaceBlock<qa_drain::CountedSource>();
        auto&                 decimator = flow.emplaceBlock<qa_drain::TailDecimator>();
        auto&                 sink      = flow.emplaceBlock<qa_drain::CountingSink>();
        source._nItems                  = nItems;
        source._maxPerCall              = nItems;
        expect(flow.connect<"out", "in">(source, decimator).has_value());
        expect(flow.connect<"out", "in">(decimator, sink).has_value());

        qa_drain::SerialScheduler      scheduler;
        gr::MsgPortIn                  fromScheduler;
        const std::vector<gr::Message> reports = qa_drain::runAndTakeDroppedTailReports(scheduler, fromScheduler, std::move(flow), decimator.unique_name);
        expect(eq(sink._nReceived, kChunks + 1UZ)) << "the epilogue did not write one sample for the partial chunk";
        expect(eq(reports.size(), 0UZ)) << "a block with an epilogue reported a dropped tail";
    };

    "a decimator whose stream ends on a whole chunk sends no dropped-tail report"_test = [] {
        constexpr std::size_t kChunks = 16UZ;
        const std::size_t     nItems  = kChunks * qa_drain::kDecimation;
        gr::Graph             flow;
        auto&                 source    = flow.emplaceBlock<qa_drain::CountedSource>();
        auto&                 decimator = flow.emplaceBlock<qa_drain::ChunkDecimator>();
        auto&                 sink      = flow.emplaceBlock<qa_drain::CountingSink>();
        source._nItems                  = nItems;
        source._maxPerCall              = nItems;
        expect(flow.connect<"out", "in">(source, decimator).has_value());
        expect(flow.connect<"out", "in">(decimator, sink).has_value());

        qa_drain::SerialScheduler      scheduler;
        gr::MsgPortIn                  fromScheduler;
        const std::vector<gr::Message> reports = qa_drain::runAndTakeDroppedTailReports(scheduler, fromScheduler, std::move(flow), decimator.unique_name);
        expect(eq(sink._nReceived, kChunks)) << "the sink does not receive one sample per chunk";
        expect(eq(reports.size(), 0UZ)) << "a stream of whole chunks reported a dropped tail";
    };

    "a decimator stopped with a partial chunk at the end of its stream sends no dropped-tail report"_test = [] {
        for (const bool stopNow : {false, true}) {
            gr::PortOut<int>         upstream;
            qa_drain::ChunkDecimator block;
            gr::PortIn<int>          downstream;
            gr::MsgPortIn            fromBlock;
            expect(fatal(upstream.connect(block.in).has_value()));
            expect(fatal(block.out.connect(downstream).has_value()));
            expect(block.msgOut.connect(fromBlock).has_value());
            block.init(std::make_shared<gr::Sequence>());
            qa_drain::publishSamples(upstream, qa_drain::kTail);
            upstream.publishEoSTag(gr::property_map{{gr::tag::END_OF_STREAM.key(), true}});

            std::ignore                   = block.settings().setStaged({{"stop_now", stopNow}});
            const gr::work::Result result = block.work();

            expect(result.status == gr::work::Status::DONE);
            expect(block.state() == STOPPED);
            expect(eq(qa_drain::takeDroppedTailReports(fromBlock, block.unique_name).size(), stopNow ? 0UZ : 1UZ)) << std::format("stop_now {}: the tail is reported without a stop request and not with one", stopNow);
        }
    };

    "a graph ending on an error does not drain"_test = [] {
        gr::Graph flow;
        auto&     source = flow.emplaceBlock<qa_drain::BurstSource>();
        auto&     relay  = flow.emplaceBlock<qa_drain::FailingRelay>();
        auto&     sink   = flow.emplaceBlock<qa_drain::CountingSink>();
        expect(flow.connect<"out", "in">(source, relay).has_value());
        expect(flow.connect<"out", "in">(relay, sink).has_value());

        qa_drain::SerialScheduler scheduler;
        expect(scheduler.exchange(std::move(flow)).has_value());
        expect(qa_drain::runWithin(scheduler, std::chrono::duration_cast<std::chrono::milliseconds>(qa_drain::kRunBound))) << "the failing graph did not end";

        expect(scheduler.state() == ERROR) << "a failing block drives the scheduler to ERROR";
        expect(eq(relay._nForwarded, qa_drain::kItemsBeforeFailure)) << "the failed block was kept running to empty its queue";
        expect(le(sink._nReceived, qa_drain::kItemsBeforeFailure));
    };

    "a tag published from processOne at sample 100 ends the call after 101 samples"_test = [] {
        constexpr std::size_t kTagAt = 100UZ;

        gr::PortIn<int>         downstream;
        qa_drain::MarkingSource source;
        expect(source.out.connect(downstream).has_value());
        source.init(std::make_shared<gr::Sequence>());
        source._tagAt = kTagAt;

        const auto result = source.work(qa_drain::kMarkedCall);
        expect(eq(result.performed_work, kTagAt + 1UZ)) << "the call ends at the sample that published the tag";

        auto out = downstream.get<gr::SpanReleasePolicy::ProcessAll>(downstream.streamReader().available());
        expect(eq(out.size(), kTagAt + 1UZ));
        std::vector<std::size_t> indices;
        for (const gr::Tag& entry : out.rawTags) {
            if (entry.map.contains("marker")) {
                indices.push_back(entry.index);
            }
        }
        expect(indices == std::vector<std::size_t>{kTagAt}) << "the tag lands at the sample that published it";
        for (std::size_t i = 0UZ; i < out.size(); ++i) {
            expect(eq(out[i], static_cast<int>(i))) << "sample " << i << " is the one processOne returned";
        }
        std::ignore = out.consume(out.size());
    };

    "a stop requested from processOne at sample 50 publishes 51 samples"_test = [] {
        constexpr std::size_t kStopAt = 50UZ;

        gr::PortIn<int>         downstream;
        qa_drain::MarkingSource source;
        expect(source.out.connect(downstream).has_value());
        source.init(std::make_shared<gr::Sequence>());
        source._stopAt = kStopAt;
        expect(source.changeStateTo(RUNNING).has_value());

        const auto result = source.work(qa_drain::kMarkedCall);
        expect(result.status == gr::work::Status::DONE);
        expect(eq(downstream.streamReader().available(), kStopAt + 1UZ)) << "the call publishes up to the sample that requested the stop";
    };
};

int main() { /* tests are statically registered */ }
