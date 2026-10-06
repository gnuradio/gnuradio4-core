#include <memory>
#include <utility>

#include <gnuradio-4.0/Block.hpp>
#include <gnuradio-4.0/BlockRegistry.hpp>
#include <gnuradio-4.0/SchedulerModel.hpp>

#include "cross_object_scheduler.hpp"

/**
 * @brief A shared object whose blocks and scheduler run beside the code of the program that loads it.
 *
 * Its blocks handle messages with code from this object. Its scheduler runs workers with code from this object.
 */
namespace gr::testing {

struct LibraryQuietSink : gr::Block<LibraryQuietSink> {
    using Description = gr::Doc<"consumes its input, from a shared object that carries no plugin interface">;

    gr::PortIn<float> in;

    GR_MAKE_REFLECTABLE(LibraryQuietSink, in);

    explicit LibraryQuietSink(gr::property_map init = {}) : gr::Block<LibraryQuietSink>(std::move(init)) {}

    void processOne(float) {}
};

const bool registeredSink [[maybe_unused]] = gr::globalBlockRegistry().insert<LibraryQuietSink>("=test::library_quiet_sink");

std::unique_ptr<gr::SchedulerModel> makeCrossObjectScheduler(gr::property_map parameters) { return std::make_unique<gr::SchedulerWrapper<CrossObjectScheduler>>(std::move(parameters)); }

const bool registeredScheduler [[maybe_unused]] = gr::globalSchedulerRegistry().insert("test::cross_object_scheduler", "", makeCrossObjectScheduler);

} // namespace gr::testing
