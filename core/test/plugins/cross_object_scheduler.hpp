#ifndef GNURADIO_TEST_CROSS_OBJECT_SCHEDULER_HPP
#define GNURADIO_TEST_CROSS_OBJECT_SCHEDULER_HPP

#include <memory>
#include <mutex>
#include <vector>

#include <gnuradio-4.0/Graph.hpp>
#include <gnuradio-4.0/Scheduler.hpp>

namespace gr::testing {

/**
 * @brief A single-threaded scheduler whose swap check any caller can run.
 *
 * A shared object and the program that loads it both compile this type, and each holds its own copy of the type's
 * code. The object registers it, so an instance from the registry runs its workers with the object's code.
 */
struct CrossObjectScheduler : gr::scheduler::SchedulerBase<CrossObjectScheduler, gr::scheduler::ExecutionPolicy::singleThreaded> {
    using Base = gr::scheduler::SchedulerBase<CrossObjectScheduler, gr::scheduler::ExecutionPolicy::singleThreaded>;
    using Base::Base;
    using Base::swapAllowedFromThisThread;

    void customInit() {
        const gr::Graph flatGraph = gr::graph::flatten(*this->_graph);
        std::lock_guard lock(this->_executionOrderMutex);
        std::lock_guard guard(this->_adoptionBlocksMutex);
        this->_adoptionBlocks.assign(1UZ, {});
        this->_executionOrder->assign(1UZ, std::vector<std::shared_ptr<gr::BlockModel>>(flatGraph.blocks().begin(), flatGraph.blocks().end()));
    }
};

} // namespace gr::testing

#endif // GNURADIO_TEST_CROSS_OBJECT_SCHEDULER_HPP
