#include <algorithm>
#include <cstddef>
#include <string>
#include <vector>

#include <gnuradio-4.0/Plugin.hpp>
#include <gnuradio-4.0/Scheduler.hpp>
#include <gnuradio-4.0/SchedulerModel.hpp>

GR_PLUGIN("rungraph Fixture Plugin", "Unknown", "MIT", "v1")

namespace fixture {

/// A block without ports that reports one error on the message plane, then fails its run or runs on without end.
struct Fault : gr::Block<Fault> {
    using Description = gr::Doc<"a block that reports one error">;

    struct Settings {
        gr::Annotated<bool, gr::Doc<"error status after the report">> fails = false;

        GR_MAKE_REFLECTABLE(Settings, fails);
    } settings;

    GR_MAKE_REFLECTABLE(Fault, settings);

    bool reported = false;

    [[nodiscard]] gr::work::Status processBulk() {
        if (!reported) {
            reported = true;
            this->emitErrorMessage("processBulk", "the fixture reports a fault");
            if (settings.fails) {
                return gr::work::Status::ERROR;
            }
        }
        return gr::work::Status::OK;
    }
};

/// A block without ports that fails in the step `fails_in` names and otherwise runs on without end.
///
/// `fails_in` is one of `applySettings`, `start` and `processBulk`, each of which throws, `work`, for a
/// processBulk() that returns ERROR without a report, or `device`, for a first processBulk() whose device fails `gain`
/// and a run that goes on. `gain` has limits, and `count` is an integer, for the values a graph file can give out of
/// range or of the wrong type.
struct Refusal : gr::Block<Refusal> {
    using Description = gr::Doc<"a block that fails in the step its setting names">;

    struct Settings {
        gr::Annotated<std::string, gr::Doc<"applySettings, start, processBulk, work or device">> fails_in;
        gr::Annotated<float, gr::Limits<gr::Min<0.f>, gr::Max<1.f>>>                             gain  = 0.5f;
        gr::Annotated<gr::Size_t>                                                                count = 1U;

        GR_MAKE_REFLECTABLE(Settings, fails_in, gain, count);
    } settings;

    GR_MAKE_REFLECTABLE(Refusal, settings);

    bool deviceFailed = false;

    void start() {
        if (settings.fails_in.value == "start") {
            throw gr::exception("the fixture refuses to start");
        }
    }

    [[nodiscard]] gr::work::Status processBulk() {
        if (settings.fails_in.value == "processBulk") {
            throw gr::exception("the fixture fails in processBulk");
        }
        if (settings.fails_in.value == "device" && !deviceFailed) {
            deviceFailed = true;
            this->fail(settings.gain, "the fixture's device lost the gain");
        }
        return settings.fails_in.value == "work" ? gr::work::Status::ERROR : gr::work::Status::OK;
    }

    void applySettings(const Settings& s, const Settings& old) {
        if (s.fails_in.value != old.fails_in.value && s.fails_in.value == "applySettings") {
            throw gr::exception("the fixture refuses its settings");
        }
    }
};

/// A source of `n_samples` samples that repeat `pattern`, or zeros for an empty one. `largest_call` holds the most
/// samples one processBulk() call published.
struct ChunkSource : gr::Block<ChunkSource> {
    using Description = gr::Doc<"a source that records the largest call it made">;

    gr::PortOut<float> out;

    struct Settings {
        gr::Annotated<gr::Size_t, gr::Doc<"samples to publish before the stream ends">> n_samples = 1000U;
        gr::Annotated<std::vector<float>, gr::Doc<"the samples the stream repeats">>    pattern   = std::vector<float>{1.f};

        GR_MAKE_REFLECTABLE(Settings, n_samples, pattern);
    } settings;

    struct Status {
        gr::Annotated<gr::Size_t, gr::Doc<"most samples one call published">> largest_call = 0U;

        GR_MAKE_REFLECTABLE(Status, largest_call);
    } status;

    GR_MAKE_REFLECTABLE(ChunkSource, out, settings, status);

    gr::Size_t nPublished = 0U;

    void start() {
        nPublished          = 0U;
        status.largest_call = 0U;
    }

    [[nodiscard]] gr::work::Status processBulk(gr::OutputSpanLike auto& outSpan) {
        const std::vector<float>& pattern = settings.pattern.value;
        const std::size_t         count   = std::min(outSpan.size(), static_cast<std::size_t>(settings.n_samples.value - nPublished));
        for (std::size_t i = 0UZ; i < count; ++i) {
            outSpan[i] = pattern.empty() ? 0.f : pattern[(nPublished + i) % pattern.size()];
        }
        outSpan.publish(count);
        nPublished += static_cast<gr::Size_t>(count);
        status.largest_call = std::max(status.largest_call.value, static_cast<gr::Size_t>(count));
        return nPublished >= settings.n_samples.value ? gr::work::Status::DONE : gr::work::Status::OK;
    }
};

/// A block that passes its input through. `largest_call` holds the most input samples one processBulk() call took.
struct ChunkRecorder : gr::Block<ChunkRecorder> {
    using Description = gr::Doc<"a 1:1 block that records the largest call it took">;

    gr::PortIn<float>  in;
    gr::PortOut<float> out;

    struct Status {
        gr::Annotated<gr::Size_t, gr::Doc<"most input samples one call took">> largest_call = 0U;

        GR_MAKE_REFLECTABLE(Status, largest_call);
    } status;

    GR_MAKE_REFLECTABLE(ChunkRecorder, in, out, status);

    void start() { status.largest_call = 0U; }

    [[nodiscard]] gr::work::Status processBulk(gr::InputSpanLike auto& inSpan, gr::OutputSpanLike auto& outSpan) {
        std::ranges::copy(inSpan, outSpan.begin());
        status.largest_call = std::max(status.largest_call.value, static_cast<gr::Size_t>(inSpan.size()));
        return gr::work::Status::OK;
    }
};

/// A decimator by four that keeps the first sample of each chunk of four. `largest_call` holds the largest input it took in one call.
struct ChunkDecimator : gr::Block<ChunkDecimator, gr::Resampling<4U, 1U, true>> {
    using Description = gr::Doc<"a decimator by four that records the largest call it took">;

    gr::PortIn<float>  in;
    gr::PortOut<float> out;

    struct Status {
        gr::Annotated<gr::Size_t, gr::Doc<"most input samples one call took">> largest_call = 0U;

        GR_MAKE_REFLECTABLE(Status, largest_call);
    } status;

    GR_MAKE_REFLECTABLE(ChunkDecimator, in, out, status);

    void start() { status.largest_call = 0U; }

    [[nodiscard]] gr::work::Status processBulk(gr::InputSpanLike auto& inSpan, gr::OutputSpanLike auto& outSpan) {
        for (std::size_t i = 0UZ; i < outSpan.size(); ++i) {
            outSpan[i] = inSpan[i * 4UZ];
        }
        status.largest_call = std::max(status.largest_call.value, static_cast<gr::Size_t>(inSpan.size()));
        return gr::work::Status::OK;
    }
};

/// A sink. `largest_call` holds the most samples one processBulk() call took, and `received` counts them all.
struct ChunkSink : gr::Block<ChunkSink> {
    using Description = gr::Doc<"a sink that records the largest call it took">;

    gr::PortIn<float> in;

    struct Status {
        gr::Annotated<gr::Size_t, gr::Doc<"most samples one call took">> largest_call = 0U;
        gr::Annotated<gr::Size_t, gr::Doc<"samples taken">>              received     = 0U;

        GR_MAKE_REFLECTABLE(Status, largest_call, received);
    } status;

    GR_MAKE_REFLECTABLE(ChunkSink, in, status);

    void start() {
        status.largest_call = 0U;
        status.received     = 0U;
    }

    [[nodiscard]] gr::work::Status processBulk(gr::InputSpanLike auto& inSpan) {
        status.largest_call = std::max(status.largest_call.value, static_cast<gr::Size_t>(inSpan.size()));
        status.received     = static_cast<gr::Size_t>(status.received.value + inSpan.size());
        return gr::work::Status::OK;
    }
};

/// A scheduler whose constructor refuses to build it, whatever settings it is given.
struct UnbuildableScheduler : gr::scheduler::Simple<gr::scheduler::ExecutionPolicy::singleThreaded> {
    explicit UnbuildableScheduler(const gr::property_map& = {}) : gr::scheduler::Simple<gr::scheduler::ExecutionPolicy::singleThreaded>({}) { throw gr::exception("the fixture scheduler cannot be built"); }
};

} // namespace fixture

const bool registeredFault [[maybe_unused]] = grPluginBlockRegistry().insert<fixture::Fault>("=test::fault");

const bool registeredRefusal [[maybe_unused]] = grPluginBlockRegistry().insert<fixture::Refusal>("=test::refusal");

const bool registeredChunkSource [[maybe_unused]] = grPluginBlockRegistry().insert<fixture::ChunkSource>("=test::chunk_source");

const bool registeredChunkRecorder [[maybe_unused]] = grPluginBlockRegistry().insert<fixture::ChunkRecorder>("=test::chunk_recorder");

const bool registeredChunkDecimator [[maybe_unused]] = grPluginBlockRegistry().insert<fixture::ChunkDecimator>("=test::chunk_decimator");

const bool registeredChunkSink [[maybe_unused]] = grPluginBlockRegistry().insert<fixture::ChunkSink>("=test::chunk_sink");

const bool registeredUnbuildable [[maybe_unused]] = [](auto& registry) {
    registry.template insert<fixture::UnbuildableScheduler>();
    return true;
}(static_cast<gr::SchedulerRegistry&>(grPluginInstance()));
