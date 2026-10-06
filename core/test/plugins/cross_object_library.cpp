#include <memory>
#include <utility>

#include <gnuradio-4.0/BlockRegistry.hpp>
#include <gnuradio-4.0/SchedulerModel.hpp>

#include "cross_object_scheduler.hpp"

/**
 * @brief A shared object whose scheduler runs beside the code of the program that loads it.
 *
 * Its scheduler runs workers with code from this object.
 */
namespace gr::testing {

std::unique_ptr<gr::SchedulerModel> makeCrossObjectScheduler(gr::property_map parameters) { return std::make_unique<gr::SchedulerWrapper<CrossObjectScheduler>>(std::move(parameters)); }

const bool registeredScheduler [[maybe_unused]] = gr::globalSchedulerRegistry().insert("test::cross_object_scheduler", "", makeCrossObjectScheduler);

} // namespace gr::testing
