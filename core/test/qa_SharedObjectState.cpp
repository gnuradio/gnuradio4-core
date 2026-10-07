#include <boost/ut.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <memory>
#include <memory_resource>
#include <print>
#include <set>
#include <string>
#include <string_view>
#include <thread>
#include <tuple>
#include <vector>

#include <gnuradio-4.0/Block.hpp>
#include <gnuradio-4.0/BlockRegistry.hpp>
#include <gnuradio-4.0/CircularBuffer.hpp>
#include <gnuradio-4.0/ComputeDomain.hpp>
#include <gnuradio-4.0/Graph.hpp>
#include <gnuradio-4.0/Message.hpp>
#include <gnuradio-4.0/PluginLoader.hpp>
#include <gnuradio-4.0/Scheduler.hpp>
#include <gnuradio-4.0/SchedulerModel.hpp>
#include <gnuradio-4.0/thread/thread_pool.hpp>

#include "build_configure.hpp"
#include "plugins/cross_object_probe.hpp"
#include "plugins/cross_object_scheduler.hpp"

// The test loads a shared object that compiles the core's headers with code of its own, as a plugin or a block library
// does. Its code and this program's code reach one value of each kind for the whole program.

namespace qa_shared_object_state {

constexpr std::string_view kLibraryProbe         = "test::cross_object_probe";
constexpr std::string_view kLibraryScheduler     = "test::cross_object_scheduler";
constexpr std::string_view kLibraryMultiThreaded = "test::cross_object_multi_threaded";

// loads the test's shared object once. The loader keeps an object that registered entries mapped after the loader is
// gone
[[nodiscard]] bool loadCrossObjectLibrary() {
    static const bool loaded = [] {
        const std::vector<std::string> directories{std::string(TESTS_BINARY_PATH) + "/cross_object_library"};
        gr::PluginLoader               loader(gr::globalBlockRegistry(), gr::globalSchedulerRegistry(), directories);
        return gr::globalBlockRegistry().contains(kLibraryProbe) && gr::globalSchedulerRegistry().contains(kLibraryScheduler) && gr::globalSchedulerRegistry().contains(kLibraryMultiThreaded);
    }();
    return loaded;
}

template<typename TPredicate>
[[nodiscard]] bool awaitCondition(TPredicate satisfied) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!satisfied()) {
        if (std::chrono::steady_clock::now() >= deadline) {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return true;
}

[[nodiscard]] gr::testing::CrossObjectProbe& libraryProbe(const std::shared_ptr<gr::BlockModel>& block) { return *static_cast<gr::testing::CrossObjectProbe*>(block->raw()); }

std::atomic<std::size_t> gTickCalls{0UZ};

// publishes one sample per call while its output has room, and never finishes
struct TickSource : gr::Block<TickSource> {
    gr::PortOut<float> out;

    GR_MAKE_REFLECTABLE(TickSource, out);

    gr::work::Status processBulk(gr::OutputSpanLike auto& outSpan) {
        gTickCalls.fetch_add(1UZ);
        const std::size_t nPublish = std::min(outSpan.size(), 1UZ);
        outSpan.publish(nPublish);
        return nPublish == 0UZ ? gr::work::Status::INSUFFICIENT_OUTPUT_ITEMS : gr::work::Status::OK;
    }
};

struct DiscardSink : gr::Block<DiscardSink> {
    gr::PortIn<float> in;

    GR_MAKE_REFLECTABLE(DiscardSink, in);

    void processOne(float) {}
};

} // namespace qa_shared_object_state

using namespace qa_shared_object_state;

const boost::ut::suite<"values shared with a shared object"> sharedObjectStateTests = [] {
    using namespace boost::ut;

    // The program and the shared object each construct the same block type. Every instance takes an id and a name that
    // no other instance of the type holds
    "a block type built into the program and into a shared object takes distinct ids and names"_test = [] {
        expect(fatal(loadCrossObjectLibrary())) << "the shared object did not load";
        std::vector<std::shared_ptr<gr::BlockModel>>                fromLibrary;
        std::vector<std::unique_ptr<gr::testing::CrossObjectProbe>> fromProgram;
        std::set<std::size_t>                                       ids;
        std::set<std::string>                                       names;
        for (std::size_t i = 0UZ; i < 3UZ; ++i) {
            fromLibrary.push_back(gr::globalBlockRegistry().create(kLibraryProbe, gr::property_map{}));
            expect(fatal(fromLibrary.back() != nullptr)) << "the shared object's probe was not created";
            fromProgram.push_back(std::make_unique<gr::testing::CrossObjectProbe>());
            const gr::testing::CrossObjectProbe& library = libraryProbe(fromLibrary.back());
            const gr::testing::CrossObjectProbe& program = *fromProgram.back();
            std::println("probe from the shared object: {}, from the program: {}", library.unique_name.value(), program.unique_name.value());
            ids.insert(library.unique_id.value());
            ids.insert(program.unique_id.value());
            names.insert(library.unique_name.value());
            names.insert(program.unique_name.value());
        }
        expect(eq(ids.size(), 6UZ)) << "two instances hold one unique_id";
        expect(eq(names.size(), 6UZ)) << "two instances hold one unique_name";
    };

    // The schedulers come from the shared object, whose code asks the thread pool manager for the default pool when it
    // constructs a scheduler. The multi-threaded one runs its workers on the program's default pool, and no second set
    // of default pools starts
    "a scheduler from a shared object runs on the program's default thread pool"_test = [] {
        expect(fatal(loadCrossObjectLibrary())) << "the shared object did not load";
        const std::shared_ptr<gr::thread_pool::TaskExecutor> programPool    = gr::thread_pool::Manager::defaultCpuPool();
        const std::size_t                                    nThreadsBefore = gr::thread_pool::getTotalThreadCount();

        std::unique_ptr<gr::SchedulerModel> model = gr::globalSchedulerRegistry().create(kLibraryScheduler, gr::property_map{});
        expect(fatal(model != nullptr)) << "the shared object's scheduler was not created";
        const std::size_t nThreadsAfter = gr::thread_pool::getTotalThreadCount();
        auto*             scheduler     = static_cast<gr::testing::CrossObjectScheduler*>(model->asBlockModel()->raw());

        std::unique_ptr<gr::SchedulerModel> multiThreaded = gr::globalSchedulerRegistry().create(kLibraryMultiThreaded, gr::property_map{});
        expect(fatal(multiThreaded != nullptr)) << "the shared object's multi-threaded scheduler was not created";
        gr::Graph flow;
        auto&     source = flow.emplaceBlock<TickSource>();
        auto&     sink   = flow.emplaceBlock<DiscardSink>();
        expect(fatal(flow.connect<"out", "in">(source, sink).has_value()));
        multiThreaded->setGraph(std::move(flow));
        gr::BlockModel& multiThreadedBlock = *multiThreaded->asBlockModel();
        gr::MsgPortOut  toScheduler;
        expect(fatal(toScheduler.connect(*multiThreadedBlock.msgIn).has_value()));

        const std::size_t nTasksBefore = programPool->numTasksRunning();
        gTickCalls.store(0UZ);
        std::thread runner([&multiThreaded] { std::ignore = multiThreaded->runAndWait(); });
        expect(awaitCondition([] { return gTickCalls.load() > 0UZ; })) << "the run made no work() call";
        const std::size_t nTasksRunning = programPool->numTasksRunning();
        gr::sendMessage<gr::message::Command::Set>(toScheduler, std::string(multiThreadedBlock.uniqueName()), gr::block::property::kLifeCycleState, gr::property_map{{"state", std::string("REQUESTED_STOP")}});
        runner.join();

        std::println("schedulers from a shared object: same manager {}, same pool {}, {} threads started by a creation, the program's pool ran {} tasks before the run and {} during it", scheduler->manager == &gr::thread_pool::Manager::instance(), scheduler->_pool.get() == programPool.get(), nThreadsAfter > nThreadsBefore ? nThreadsAfter - nThreadsBefore : 0UZ, nTasksBefore, nTasksRunning);
        expect(scheduler->manager == &gr::thread_pool::Manager::instance()) << "the shared object's code reached a thread pool manager of its own";
        expect(scheduler->_pool.get() == programPool.get()) << "the scheduler holds a default pool other than the program's";
        expect(lt(nThreadsAfter, nThreadsBefore + programPool->minThreads())) << "creating the scheduler started a second default pool";
        expect(gt(nTasksRunning, nTasksBefore)) << "the program's default pool did not run the scheduler's workers";
    };

    // The program and the shared object both read the default double-mapped memory resource. The two allocators over it
    // compare equal
    "a shared object's default double-mapped resource is the program's"_test = [] {
        expect(fatal(loadCrossObjectLibrary())) << "the shared object did not load";
        const std::shared_ptr<gr::BlockModel> block = gr::globalBlockRegistry().create(kLibraryProbe, gr::property_map{});
        expect(fatal(block != nullptr)) << "the shared object's probe was not created";
        const std::pmr::polymorphic_allocator<float> libraryAllocator(libraryProbe(block).defaultResource);
        const std::pmr::polymorphic_allocator<float> programAllocator = gr::double_mapped_memory_resource::allocator<float>();
        expect(libraryProbe(block).defaultResource == gr::double_mapped_memory_resource::defaultAllocator()) << "the shared object's code holds a default resource of its own";
        expect(libraryAllocator == programAllocator) << "the default allocators of the shared object and the program compare unequal";
    };

    // The program registers a compute provider, and the shared object's code resolves the provider's domain
    "a compute provider that the program registers resolves in a shared object's code"_test = [] {
        expect(fatal(loadCrossObjectLibrary())) << "the shared object did not load";
        gr::ComputeRegistry::instance().register_provider(gr::testing::kCrossObjectDomain.backend, [](const gr::ComputeDomain&, void*) { return std::pmr::null_memory_resource(); });
        expect(gr::ComputeRegistry::instance().tryResolve(gr::testing::kCrossObjectDomain) == std::pmr::null_memory_resource()) << "the program did not resolve its own provider";
        const std::shared_ptr<gr::BlockModel> block = gr::globalBlockRegistry().create(kLibraryProbe, gr::property_map{});
        expect(fatal(block != nullptr)) << "the shared object's probe was not created";
        expect(libraryProbe(block).providerResource == std::pmr::null_memory_resource()) << "the shared object's code did not find the program's provider";
    };
};

int main() { /* tests are statically registered */ }
