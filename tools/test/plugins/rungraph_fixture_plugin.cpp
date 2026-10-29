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

/// A scheduler whose constructor refuses to build it, whatever settings it is given.
struct UnbuildableScheduler : gr::scheduler::Simple<gr::scheduler::ExecutionPolicy::singleThreaded> {
    explicit UnbuildableScheduler(const gr::property_map& = {}) : gr::scheduler::Simple<gr::scheduler::ExecutionPolicy::singleThreaded>({}) { throw gr::exception("the fixture scheduler cannot be built"); }
};

} // namespace fixture

const bool registeredFault [[maybe_unused]] = grPluginBlockRegistry().insert<fixture::Fault>("=test::fault");

const bool registeredUnbuildable [[maybe_unused]] = [](auto& registry) {
    registry.template insert<fixture::UnbuildableScheduler>();
    return true;
}(static_cast<gr::SchedulerRegistry&>(grPluginInstance()));
