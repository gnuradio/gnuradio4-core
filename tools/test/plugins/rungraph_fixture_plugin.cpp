#include <string>

#include <gnuradio-4.0/Plugin.hpp>
#include <gnuradio-4.0/Scheduler.hpp>
#include <gnuradio-4.0/SchedulerModel.hpp>

GR_PLUGIN("rungraph Fixture Plugin", "Unknown", "MIT", "v1")

namespace fixture {

/// A block without ports that reports one error on the message plane, then fails its run or runs on without end.
struct Fault : gr::Block<Fault> {
    using Description = gr::Doc<"a block that reports one error">;

    gr::Annotated<bool, "fails", gr::Doc<"error status after the report">> fails = false;

    GR_MAKE_REFLECTABLE(Fault, fails);

    bool _reported = false;

    [[nodiscard]] gr::work::Status processBulk() {
        if (!_reported) {
            _reported = true;
            this->emitErrorMessage("processBulk", "the fixture reports a fault");
            if (fails) {
                return gr::work::Status::ERROR;
            }
        }
        return gr::work::Status::OK;
    }
};

/// A block without ports that fails in the step `fails_in` names and otherwise runs on without end.
///
/// `fails_in` is one of `settingsChanged`, `start` and `processBulk`, each of which throws, or `work`, for a
/// processBulk() that returns ERROR without a report. `gain` has limits, and `count` is an integer, for the values a
/// graph file can give out of range or of the wrong type.
struct Refusal : gr::Block<Refusal> {
    using Description = gr::Doc<"a block that fails in the step its setting names">;

    gr::Annotated<std::string, "fails_in", gr::Doc<"settingsChanged, start, processBulk or work">> fails_in;
    gr::Annotated<float, "gain", gr::Limits<0.f, 1.f>>                                             gain  = 0.5f;
    gr::Annotated<gr::Size_t, "count">                                                             count = 1U;

    GR_MAKE_REFLECTABLE(Refusal, fails_in, gain, count);

    void start() {
        if (fails_in.value == "start") {
            throw gr::exception("the fixture refuses to start");
        }
    }

    [[nodiscard]] gr::work::Status processBulk() {
        if (fails_in.value == "processBulk") {
            throw gr::exception("the fixture fails in processBulk");
        }
        return fails_in.value == "work" ? gr::work::Status::ERROR : gr::work::Status::OK;
    }

    void settingsChanged(const gr::property_map& /*oldSettings*/, const gr::property_map& newSettings) {
        if (newSettings.contains("fails_in") && fails_in.value == "settingsChanged") {
            throw gr::exception("the fixture refuses its settings");
        }
    }
};

/// A scheduler whose constructor refuses to build it, whatever settings it is given.
struct UnbuildableScheduler : gr::scheduler::Simple<gr::scheduler::ExecutionPolicy::singleThreaded> {
    explicit UnbuildableScheduler(const gr::property_map& = {}) : gr::scheduler::Simple<gr::scheduler::ExecutionPolicy::singleThreaded>({}) { throw gr::exception("the fixture scheduler cannot be built"); }
};

} // namespace fixture

const bool registeredFault [[maybe_unused]] = grPluginBlockRegistry().insert<fixture::Fault>("=test::fault");

const bool registeredRefusal [[maybe_unused]] = grPluginBlockRegistry().insert<fixture::Refusal>("=test::refusal");

const bool registeredUnbuildable [[maybe_unused]] = [](auto& registry) {
    registry.template insert<fixture::UnbuildableScheduler>();
    return true;
}(static_cast<gr::SchedulerRegistry&>(grPluginInstance()));
