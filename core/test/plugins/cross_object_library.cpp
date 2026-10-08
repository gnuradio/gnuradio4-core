#include <memory>
#include <utility>

#include <gnuradio-4.0/Block.hpp>
#include <gnuradio-4.0/BlockRegistry.hpp>
#include <gnuradio-4.0/SchedulerModel.hpp>

#include "cross_object_scheduler.hpp"

/**
 * @brief A shared object whose blocks and schedulers run beside the code of the program that loads it.
 *
 * Its blocks handle messages and publish on a scheduler's worker with code from this object. Its two schedulers run
 * workers with code from this object, the single-threaded one on the calling thread and the multi-threaded one on a
 * thread pool.
 */
namespace gr::testing {

struct LibrarySilentSource : gr::Block<LibrarySilentSource> {
    using Description = gr::Doc<"publishes nothing and never finishes, from a shared object that carries no plugin interface">;

    gr::PortOut<float> out;

    GR_MAKE_REFLECTABLE(LibrarySilentSource, out);

    explicit LibrarySilentSource(gr::property_map init = {}) : gr::Block<LibrarySilentSource>(std::move(init)) {}

    gr::work::Status processBulk(gr::OutputSpanLike auto& outSpan) {
        outSpan.publish(0UZ);
        return gr::work::Status::INSUFFICIENT_INPUT_ITEMS;
    }
};

struct LibraryQuietSink : gr::Block<LibraryQuietSink> {
    using Description = gr::Doc<"consumes its input, from a shared object that carries no plugin interface">;

    gr::PortIn<float> in;

    GR_MAKE_REFLECTABLE(LibraryQuietSink, in);

    explicit LibraryQuietSink(gr::property_map init = {}) : gr::Block<LibraryQuietSink>(std::move(init)) {}

    void processOne(float) {}
};

const bool registeredSource [[maybe_unused]] = gr::globalBlockRegistry().insert<LibrarySilentSource>("=test::library_silent_source");
const bool registeredSink [[maybe_unused]]   = gr::globalBlockRegistry().insert<LibraryQuietSink>("=test::library_quiet_sink");

std::unique_ptr<gr::SchedulerModel> makeCrossObjectScheduler(gr::property_map parameters) { return std::make_unique<gr::SchedulerWrapper<CrossObjectScheduler>>(std::move(parameters)); }

const bool registeredScheduler [[maybe_unused]] = gr::globalSchedulerRegistry().insert("test::cross_object_scheduler", "", makeCrossObjectScheduler);

std::unique_ptr<gr::SchedulerModel> makeMultiThreadedScheduler(gr::property_map parameters) { return std::make_unique<gr::SchedulerWrapper<gr::scheduler::Simple<gr::scheduler::ExecutionPolicy::multiThreaded>>>(std::move(parameters)); }

const bool registeredMultiThreaded [[maybe_unused]] = gr::globalSchedulerRegistry().insert("test::cross_object_multi_threaded", "", makeMultiThreadedScheduler);

} // namespace gr::testing
