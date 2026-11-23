#include "ShippedSchedulers.hpp"

#include <gnuradio-4.0/BlockRegistry.hpp>
#include <gnuradio-4.0/Scheduler.hpp>
#include <gnuradio-4.0/SchedulerModel.hpp>

#include <memory>
#include <mutex>
#include <string_view>
#include <tuple>
#include <utility>

namespace gr::detail {

namespace {

template<typename TScheduler>
[[nodiscard]] std::unique_ptr<SchedulerModel> makeScheduler(property_map parameters) {
    return std::make_unique<SchedulerWrapper<TScheduler>>(std::move(parameters));
}

template<typename TScheduler>
void insertUnlessHeld(SchedulerRegistry& registry, std::string_view key) {
    if (!registry.contains(key)) {
        std::ignore = registry.insert(key, "", makeScheduler<TScheduler>);
    }
}

} // namespace

void registerShippedSchedulers() {
    static std::once_flag registered;
    std::call_once(registered, [] {
        using scheduler::ExecutionPolicy;
        using scheduler::Simple;
        SchedulerRegistry& registry = globalSchedulerRegistry();
        insertUnlessHeld<Simple<ExecutionPolicy::single_threaded>>(registry, kDefaultSchedulerKey);
        insertUnlessHeld<Simple<ExecutionPolicy::multi_threaded>>(registry, "gr::scheduler::Simple<multi_threaded>");
        insertUnlessHeld<Simple<ExecutionPolicy::single_threaded_blocking>>(registry, "gr::scheduler::Simple<single_threaded_blocking>");
        insertUnlessHeld<Simple<ExecutionPolicy::multi_threaded_blocking>>(registry, "gr::scheduler::Simple<multi_threaded_blocking>");
    });
}

} // namespace gr::detail
