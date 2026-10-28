#ifndef GNURADIO_SHIPPED_SCHEDULERS_HPP
#define GNURADIO_SHIPPED_SCHEDULERS_HPP

#include <string_view>

namespace gr::detail {

/// the key of the scheduler a graph runs on when the caller names none
inline constexpr std::string_view kDefaultSchedulerKey = "gr::scheduler::Simple<singleThreaded>";

/**
 * @brief Registers core's `Simple` scheduler into `gr::globalSchedulerRegistry()` under one key per execution policy.
 *
 * The keys are `gr::scheduler::Simple<singleThreaded>`, `<multiThreaded>`, `<singleThreadedBlocking>` and
 * `<multiThreadedBlocking>`. The key names the policy in words, where the type name spells it as a cast of an integer.
 * Each scheduler is registered under that key alone. A listing of the registry names it once.
 *
 * The first call registers, and a later call does nothing. A key the registry already holds keeps its entry.
 */
void registerShippedSchedulers();

} // namespace gr::detail

#endif // GNURADIO_SHIPPED_SCHEDULERS_HPP
