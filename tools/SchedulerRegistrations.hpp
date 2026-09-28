#ifndef GNURADIO_TOOLS_SCHEDULERREGISTRATIONS_HPP
#define GNURADIO_TOOLS_SCHEDULERREGISTRATIONS_HPP

#include <memory>
#include <string_view>
#include <utility>

#include <gnuradio-4.0/BlockRegistry.hpp>
#include <gnuradio-4.0/Scheduler.hpp>
#include <gnuradio-4.0/SchedulerModel.hpp>

namespace gr::tools {

/// the key of the scheduler a tool runs a graph with when the caller names none
inline constexpr std::string_view kDefaultScheduler = "gr::scheduler::Simple<singleThreaded>";

template<typename TScheduler>
[[nodiscard]] std::unique_ptr<gr::SchedulerModel> makeScheduler(gr::property_map parameters) {
    return std::make_unique<gr::SchedulerWrapper<TScheduler>>(std::move(parameters));
}

/**
 * @brief Registers core's `Simple` scheduler under one key per execution policy.
 *
 * The key names the policy in words, where the type name spells it as a cast of an integer. Each scheduler is
 * registered under that key alone, so a listing of the registry names it once.
 */
inline void registerSchedulers(gr::SchedulerRegistry& registry) {
    using gr::scheduler::ExecutionPolicy;
    using gr::scheduler::Simple;
    registry.insert(kDefaultScheduler, "", makeScheduler<Simple<ExecutionPolicy::singleThreaded>>);
    registry.insert("gr::scheduler::Simple<multiThreaded>", "", makeScheduler<Simple<ExecutionPolicy::multiThreaded>>);
    registry.insert("gr::scheduler::Simple<singleThreadedBlocking>", "", makeScheduler<Simple<ExecutionPolicy::singleThreadedBlocking>>);
}

} // namespace gr::tools

#endif // GNURADIO_TOOLS_SCHEDULERREGISTRATIONS_HPP
