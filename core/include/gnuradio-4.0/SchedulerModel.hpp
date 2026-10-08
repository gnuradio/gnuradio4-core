#ifndef GNURADIO_SCHEDULER_MODEL_HPP
#define GNURADIO_SCHEDULER_MODEL_HPP

#include <gnuradio-4.0/BlockModel.hpp>

#include <memory>
#include <thread>
#include <vector>

namespace gr {

// Graph.hpp is not parsed here. SchedulerModel names gr::Graph only through a reference, and
// SchedulerWrapper is a template whose base and body are instantiated at the point of use -- which
// is a translation unit that builds a scheduler and therefore includes Graph.hpp already. Every
// block header reaches this file through BlockRegistry.hpp for the GR_REGISTER_BLOCK marker, so the
// graph machinery would otherwise be parsed by every translation unit that mentions a block.
struct Graph;

template<typename TSelf, typename TSubGraph>
class GraphWrapper;

class SchedulerModel {
public:
    SchedulerModel() = default;

public:
    SchedulerModel(const SchedulerModel&)             = delete;
    SchedulerModel& operator=(const SchedulerModel&)  = delete;
    SchedulerModel(SchedulerModel&& other)            = delete;
    SchedulerModel& operator=(SchedulerModel&& other) = delete;

    virtual ~SchedulerModel() = default;

    virtual void        setGraph(gr::Graph&&) = 0;
    virtual BlockModel* asBlockModel()        = 0;

    static std::shared_ptr<BlockModel> asBlockModelPtr(std::shared_ptr<SchedulerModel> ptr) {
        if (!ptr) {
            return {};
        }

        return std::shared_ptr<BlockModel>(ptr, ptr->asBlockModel());
    }

    // starts the scheduler on a thread of its own. A stopped or failed scheduler is initialized again first, and a
    // running one stays as it is. The error says why the start is refused. A start that fails on the scheduler's
    // thread ends it in ERROR.
    [[nodiscard]] virtual std::expected<void, Error> start() = 0;
    virtual void                                     stop()  = 0;

    // an adopted scheduler shares its parent's processing pool, so its start reports what keeps it from running
    // instead of leaving a worker queued behind the threads the parent holds
    virtual std::expected<void, Error> startAdopted() = 0;

    // runs the graph and returns when the run ends by itself, on a requested stop or on an error. The error is the
    // result.
    virtual std::expected<void, Error> runAndWait() = 0;

    [[nodiscard]] virtual bool workerStarted() = 0;

    // why the latest start could not complete. The next start clears it. A read is ordered once the scheduler reads
    // ERROR.
    [[nodiscard]] virtual std::optional<Error> startError() const = 0;

    virtual void requestWorkQuiescence() = 0;
    virtual void releaseWorkQuiescence() = 0;
};

// A scheduler that holds a nested scheduler and exports its ports hands it the progress sequences of the graphs beyond
// the holder. A wrapper without this interface advances only the sequence of the graph that holds it.
class OuterProgressReceiver {
public:
    OuterProgressReceiver()                                        = default;
    OuterProgressReceiver(const OuterProgressReceiver&)            = delete;
    OuterProgressReceiver& operator=(const OuterProgressReceiver&) = delete;
    virtual ~OuterProgressReceiver()                               = default;

    virtual void setOuterProgress(std::vector<std::shared_ptr<gr::Sequence>> outerProgress) = 0;
};

template<BlockLike TScheduler>
class SchedulerWrapper : public GraphWrapper<TScheduler, gr::Graph>, public SchedulerModel, public OuterProgressReceiver {
    static_assert(std::is_same_v<TScheduler, std::remove_reference_t<TScheduler>>);

public:
    explicit SchedulerWrapper(const gr::property_map& props = {}) //
        : GraphWrapper<TScheduler, gr::Graph>(props) {}

    SchedulerWrapper(const SchedulerWrapper& other)            = delete;
    SchedulerWrapper(SchedulerWrapper&& other)                 = delete;
    SchedulerWrapper& operator=(const SchedulerWrapper& other) = delete;
    SchedulerWrapper& operator=(SchedulerWrapper&& other)      = delete;

    // members are destroyed before bases, so a joinable _schedulerThread here would terminate the process
    ~SchedulerWrapper() override { stop(); }

    // A graph calls init() on each block it adds, and a scheduler calls it on each nested scheduler at its start. The
    // scheduler's own init() takes no progress sequence and runs when the scheduler initializes its graph. The wrapper
    // stores the given sequence in the scheduler's progress member. The scheduler advances that sequence after each
    // pass in which a block with an exported port moves samples.
    void init(std::shared_ptr<gr::Sequence> progress, std::string_view /*ioThreadPool*/ = gr::thread_pool::kDefaultIoPoolId) override { this->blockRef().progress = std::move(progress); }

    void setOuterProgress(std::vector<std::shared_ptr<gr::Sequence>> outerProgress) final { this->blockRef().setOuterProgress(std::move(outerProgress)); }

    void setGraph(gr::Graph&& graph) final { std::ignore = this->blockRef().exchange(std::move(graph)); }

    BlockModel* asBlockModel() final { return static_cast<BlockModel*>(this); }

    [[nodiscard]] std::expected<void, Error> start() override { return startOnOwnThread(false); }

    std::expected<void, Error> startAdopted() override { return startOnOwnThread(true); }

    std::expected<void, Error> runAndWait() override { return this->blockRef().runAndWait(); }

    [[nodiscard]] bool workerStarted() override { return this->blockRef().workerStarted(); }

    // a scheduler whose start or run ended in ERROR on its own thread or on its workers returns ERROR to the scheduler
    // that runs it. That run fails as it does for a block's ERROR.
    [[nodiscard]] work::Result work(std::size_t requestedWork = undefined_size) override {
        if (this->blockRef().state() == gr::lifecycle::State::ERROR) {
            return {requestedWork, 0UZ, work::Status::ERROR};
        }
        return GraphWrapper<TScheduler, gr::Graph>::work(requestedWork);
    }

    [[nodiscard]] std::optional<Error> startError() const override { return this->blockRef().startError(); }

    void requestWorkQuiescence() override { this->blockRef().requestWorkQuiescence(); }
    void releaseWorkQuiescence() override { this->blockRef().releaseWorkQuiescence(); }

    void stop() override {
        if (this->blockRef().changeStateTo(gr::lifecycle::State::REQUESTED_STOP)) {
            // transitions to stopped/error do not fail
            std::ignore = this->blockRef().changeStateTo(gr::lifecycle::State::STOPPED);
        } else {
            std::ignore = this->blockRef().changeStateTo(gr::lifecycle::State::ERROR);
        }

        if (_schedulerThread.joinable()) {
            _schedulerThread.join();
        }
    }

    std::thread _schedulerThread;

private:
    std::expected<void, Error> startOnOwnThread(bool requireWorkerCapacity) {
        using enum gr::lifecycle::State;
        auto& sched = this->blockRef();

        if (sched.state() == RUNNING) {
            return {};
        }

        if (_schedulerThread.joinable()) { // a previous run may have finished without a stop()
            _schedulerThread.join();
        }

        if (const gr::lifecycle::State state = sched.state(); state == IDLE || state == STOPPED || state == ERROR) {
            if (auto initialized = sched.changeStateTo(INITIALISED); !initialized.has_value()) {
                return initialized;
            }
        }

        if (sched.state() != INITIALISED) {
            return std::unexpected(Error(std::format("sub-scheduler '{}' is {} and cannot be started", sched.unique_name, gr::meta::enumName(sched.state()).value_or(""))));
        }

        if (std::string_view(sched.poolName.value) == gr::thread_pool::kDefaultCpuPoolId) {
            std::ignore = sched.settings().set({{"poolName", std::string(gr::thread_pool::kDefaultIoPoolId)}});
            std::ignore = sched.settings().applyStagedParameters();
        }

        if (requireWorkerCapacity) {
            if (auto capacity = sched.checkWorkerCapacity(); !capacity.has_value()) {
                return capacity;
            }
        }

        _schedulerThread = std::thread([&sched] {
            // runs the scheduler's start(). Under a single-threaded policy start() returns when the run ends, and under
            // the others once the workers are queued. A stop that claims the transition first leaves the stop's state.
            // Any other error fails the start and reaches the scheduler that runs this one.
            if (auto started = sched.changeStateTo(RUNNING); !started.has_value() && !gr::lifecycle::isShuttingDown(sched.state())) {
                sched.failStartAndReport(std::move(started.error()));
            }
        });
        return {};
    }
};

} // namespace gr

#endif // GNURADIO_BLOCK_SCHEDULER_HPP
