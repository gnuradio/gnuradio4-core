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
        insertUnlessHeld<Simple<ExecutionPolicy::singleThreaded>>(registry, kDefaultSchedulerKey);
        insertUnlessHeld<Simple<ExecutionPolicy::multiThreaded>>(registry, "gr::scheduler::Simple<multiThreaded>");
        insertUnlessHeld<Simple<ExecutionPolicy::singleThreadedBlocking>>(registry, "gr::scheduler::Simple<singleThreadedBlocking>");
        insertUnlessHeld<Simple<ExecutionPolicy::multiThreadedBlocking>>(registry, "gr::scheduler::Simple<multiThreadedBlocking>");
    });
}

} // namespace gr::detail
