#include <atomic>
#include <cassert>
#include <functional>
#include <future>
#include <gnuradio-4.0/Scheduler.hpp>
#include <iostream>
#include <thread>
#ifdef NDEBUG
#error composition qualification requires assertions
#endif

namespace qa_composition {
struct Stats {
    std::atomic<std::size_t> samples{0}, errors{0};
    std::size_t              next = 0;
};
struct Source : gr::Block<Source> {
    gr::PortOut<float> out;
    GR_MAKE_REFLECTABLE(Source, out);
    std::size_t next = 0;
    float       processOne() noexcept { return static_cast<float>((next++) % 1048576); }
};
struct Sink : gr::Block<Sink> {
    gr::PortIn<float> in;
    GR_MAKE_REFLECTABLE(Sink, in);
    std::shared_ptr<Stats> stats = std::make_shared<Stats>();
    void                   processOne(float value) noexcept {
        if (value != static_cast<float>((stats->next++) % 1048576)) {
            ++stats->errors;
        }
        ++stats->samples;
    }
};
struct Pass : gr::Block<Pass> {
    gr::PortIn<float>  in;
    gr::PortOut<float> out;
    GR_MAKE_REFLECTABLE(Pass, in, out);
    float processOne(float value) const noexcept { return value; }
};
struct RefuseStart : gr::Block<RefuseStart> {
    gr::PortIn<float> in;
    GR_MAKE_REFLECTABLE(RefuseStart, in);
    void start() { throw gr::exception("deliberate adoption failure"); }
    void processOne(float) const noexcept {}
};
struct RefuseStop : gr::Block<RefuseStop> {
    gr::PortIn<float> in;
    GR_MAKE_REFLECTABLE(RefuseStop, in);
    struct Stats {
        std::atomic<std::size_t> samples{0}, stops{0};
    };
    std::shared_ptr<Stats> stats = std::make_shared<Stats>();
    void                   stop() {
        ++stats->stops;
        throw gr::exception("deliberate retirement failure");
    }
    void processOne(float) const noexcept { ++stats->samples; }
};
struct StopCallback : gr::Block<StopCallback> {
    gr::PortIn<float> in;
    GR_MAKE_REFLECTABLE(StopCallback, in);
    std::function<void()> onStop;
    void                  stop() { onStop(); }
    void                  processOne(float) const noexcept {}
};
template<class Predicate>
void await(Predicate predicate) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
    while (!predicate()) {
        if (std::chrono::steady_clock::now() >= deadline) {
            throw std::runtime_error("synchronization deadline exceeded");
        }
        std::this_thread::yield();
    }
}
void require(const auto& value) {
    if (!value.has_value()) {
        throw std::runtime_error(value.error().message);
    }
}

template<gr::scheduler::ExecutionPolicy policy>
void qualify() {
    using Scheduler = gr::SchedulerWrapper<gr::scheduler::Simple<policy>>;
    Scheduler           scheduler;
    gr::SchedulerModel& api = scheduler;
    gr::MsgPortOut      toScheduler;
    gr::MsgPortIn       fromScheduler;
    require(toScheduler.connect(*api.asBlockModel()->msgIn));
    require(api.asBlockModel()->msgOut->connect(fromScheduler));
    gr::Graph graph;
    auto&     source = graph.emplaceBlock<Source>();
    auto&     sink   = graph.emplaceBlock<Sink>();
    require(graph.connect<"out", "in">(source, sink));
    auto existing = sink.stats;
    api.setGraph(std::move(graph));
    require(api.start());
    await([&] { return existing->samples.load() >= 10000; });

    std::weak_ptr<Stats>                       removed;
    std::shared_ptr<Stats>                     branchStats;
    std::size_t                                attached     = 0;
    Pass*                                      branchOutput = nullptr;
    std::vector<std::weak_ptr<gr::BlockModel>> branchBlocks;
    auto                                       addition = api.compose([&](gr::CompositionEditor& edit) {
        auto& pass   = edit.emplaceBlock<Pass>();
        branchOutput = &pass;
        auto& added  = edit.emplaceBlock<Sink>();
        edit.connect(source, "out", pass, "in");
        edit.connect(pass, "out", added, "in");
        attached = edit.position("attachment", pass.in);
        assert(attached == edit.position("writer", source.out));
        added.stats->next = attached;
        branchStats       = added.stats;
        removed           = branchStats;
        auto blocks       = api.asBlockModel()->graph()->blocks();
        branchBlocks.assign(blocks.end() - 2, blocks.end());
        // A reentrant edit is refused synchronously, without running its callback.
        assert(!api.compose([](auto&) { assert(false); }));
    });
    require(addition);
    assert(addition->positions.size() == 2);
    assert(addition->positions[0].second == attached && attached >= 10000);
    await([&] { return branchStats->samples.load() >= 10000; });
    assert(branchStats->errors.load() == 0);
    auto dependent = api.compose([&](gr::CompositionEditor& edit) {
        auto& added       = edit.emplaceBlock<Sink>();
        added.stats->next = attached + branchOutput->out.streamWriter().position();
        edit.connect(*branchOutput, "out", added, "in");
    });
    require(dependent);
    assert(!api.removeBranch(addition->branch)); // the second branch still depends on it
    require(api.removeBranch(dependent->branch));
    Scheduler foreign;
    assert(!foreign.removeBranch(addition->branch));
    require(api.removeBranch(addition->branch));
    assert(!api.removeBranch(addition->branch));
    branchStats.reset();
    await([&] { return removed.expired() && std::ranges::all_of(branchBlocks, [](const auto& weak) { return weak.expired(); }); });
    const auto old = existing->samples.load();
    await([&] { return existing->samples.load() >= old + 10000; });
    assert(existing->errors.load() == 0);

    // A failed edge after a connected reader must release the whole partial branch.
    std::weak_ptr<Stats> failedReader;
    auto                 failed = api.compose([&](gr::CompositionEditor& edit) {
        auto& added  = edit.emplaceBlock<Sink>();
        failedReader = added.stats;
        edit.connect(source, "out", added, "in");
        auto& invalid = edit.emplaceBlock<Sink>();
        edit.connect(source, "missing", invalid, "in");
    });
    assert(!failed);
    await([&] { return failedReader.expired(); });

    // Adoption starts one block before a later start hook refuses the branch.
    std::weak_ptr<Stats>                       partial;
    std::vector<std::weak_ptr<gr::BlockModel>> partialBlocks;
    auto                                       refused = api.compose([&](gr::CompositionEditor& edit) {
        auto& added       = edit.emplaceBlock<Sink>();
        added.stats->next = source.out.streamWriter().position();
        partial           = added.stats;
        edit.connect(source, "out", added, "in");
        auto& bad = edit.emplaceBlock<RefuseStart>();
        edit.connect(source, "out", bad, "in");
        auto blocks = api.asBlockModel()->graph()->blocks();
        partialBlocks.assign(blocks.end() - 2, blocks.end());
    });
    assert(!refused);
    await([&] { return partial.expired() && std::ranges::all_of(partialBlocks, [](const auto& weak) { return weak.expired(); }); });
    assert(scheduler.blockRef().state() == gr::lifecycle::State::RUNNING);
    const auto afterFailure = existing->samples.load();
    await([&] { return existing->samples.load() >= afterFailure + 10000; });
    assert(existing->errors.load() == 0);

    // A native RemoveBlock queued during composition runs only after the complete branch commits.
    std::string          nativeName;
    std::weak_ptr<Stats> nativeRemoved;
    std::promise<void>   editEntered, editFinish;
    auto                 editProceed    = editFinish.get_future().share();
    auto                 nativeAddition = std::async(std::launch::async, [&] {
        return api.compose([&](gr::CompositionEditor& edit) {
            auto& added       = edit.emplaceBlock<Sink>();
            nativeName        = std::string(added.unique_name);
            nativeRemoved     = added.stats;
            added.stats->next = source.out.streamWriter().position();
            edit.connect(source, "out", added, "in");
            editEntered.set_value();
            editProceed.wait();
        });
    });
    editEntered.get_future().wait();
    gr::sendMessage<gr::message::Command::Set>(toScheduler, scheduler.blockRef().unique_name, gr::scheduler::property::kRemoveBlock, {{"uniqueName", nativeName}}, "native-removal");
    editFinish.set_value();
    auto nativeBranch = nativeAddition.get();
    require(nativeBranch);
    bool answered = false;
    await([&] {
        auto messages = fromScheduler.streamReader().get();
        for (const auto& message : messages) {
            if (message.clientRequestID == "native-removal") {
                assert(message.cmd == gr::message::Command::Final && message.data.has_value());
                assert(message.endpoint == gr::scheduler::property::kBlockRemoved);
                answered = true;
            }
        }
        assert(messages.consume(messages.size()));
        return answered;
    });
    await([&] { return nativeRemoved.expired(); });
    assert(!api.removeBranch(nativeBranch->branch));

    // Stop contends with a callback held at a promise barrier, never at a timed sleep.
    std::promise<void> entered, finish;
    auto               proceed   = finish.get_future().share();
    auto               composing = std::async(std::launch::async, [&] {
        return api.compose([&](gr::CompositionEditor& edit) {
            auto& added = edit.emplaceBlock<Sink>();
            edit.connect(source, "out", added, "in");
            entered.set_value();
            proceed.wait();
        });
    });
    entered.get_future().wait();
    auto stopping = std::async(std::launch::async, [&] { api.stop(); });
    await([&] { return scheduler.blockRef().state() != gr::lifecycle::State::RUNNING; });
    finish.set_value();
    assert(!composing.get());
    stopping.get();
    assert(existing->errors.load() == 0);
    // Handles do not keep an execution alive.
    std::cout << "{\"policy\":\"" << gr::meta::enumName(policy).value() << "\",\"attachment\":" << attached << ",\"existing_samples\":" << existing->samples.load() << ",\"continuity_errors\":" << existing->errors.load() << ",\"addition_removal_rollback_stop\":true}\n";
}

// Hold an outer quiescence across removal and stop: ordinary worker maintenance cannot run.
// Reclamation must complete before the worker count is released, without a grace-period sleep.
template<gr::scheduler::ExecutionPolicy policy>
void retireAtStop() {
    gr::SchedulerWrapper<gr::scheduler::Simple<policy>> scheduler;
    gr::SchedulerModel&                                 api = scheduler;
    gr::Graph                                           graph;
    auto&                                               source   = graph.emplaceBlock<Source>();
    auto&                                               sink     = graph.emplaceBlock<Sink>();
    auto                                                progress = sink.stats;
    require(graph.connect<"out", "in">(source, sink));
    api.setGraph(std::move(graph));
    require(api.start());
    await([&] { return progress->samples.load() > 10000; });
    std::weak_ptr<Stats> removed;
    auto                 branch = api.compose([&](gr::CompositionEditor& edit) {
        auto& added       = edit.emplaceBlock<Sink>();
        added.stats->next = source.out.streamWriter().position();
        removed           = added.stats;
        edit.connect(source, "out", added, "in");
    });
    require(branch);
    api.requestWorkQuiescence();
    require(api.removeBranch(branch->branch));
    api.stop();
    scheduler.blockRef().waitDone();
    api.releaseWorkQuiescence();
    assert(removed.expired());
}

// The callback is held at a barrier while an external caller starts a drain.
// Source cancellation must wait for the complete edit, including vector growth.
template<gr::scheduler::ExecutionPolicy policy>
void drainDuringComposition() {
    gr::SchedulerWrapper<gr::scheduler::Simple<policy>> scheduler;
    gr::Graph                                           graph;
    auto&                                               source = graph.emplaceBlock<Source>();
    auto&                                               sink   = graph.emplaceBlock<Sink>();
    auto                                                stats  = sink.stats;
    require(graph.connect<"out", "in">(source, sink));
    scheduler.setGraph(std::move(graph));
    require(scheduler.start());
    await([&] { return stats->samples.load() > 10000; });
    std::promise<void> entered, proceed, draining;
    auto               release = proceed.get_future().share();
    auto               editing = std::async(std::launch::async, [&] {
        return scheduler.compose([&](gr::CompositionEditor& edit) {
            entered.set_value();
            release.wait();
            for (int i = 0; i < 32; ++i) {
                auto& added       = edit.emplaceBlock<Sink>();
                added.stats->next = source.out.streamWriter().position();
                edit.connect(source, "out", added, "in");
            }
        });
    });
    entered.get_future().wait();
    auto drain = std::async(std::launch::async, [&] {
        draining.set_value();
        return scheduler.stopAfterDrain(std::chrono::seconds(5));
    });
    draining.get_future().wait();
    const bool waited = drain.wait_for(std::chrono::milliseconds(20)) == std::future_status::timeout;
    proceed.set_value();
    require(editing.get());
    const bool drained = drain.get();
    assert(waited && drained);
    assert(scheduler.state() == gr::lifecycle::State::STOPPED);
    assert(stats->errors.load() == 0);
}

// A stop hook error is reported, but fully retiring its block must preserve the running stream.
// Exercise both explicit removal and rollback after a later block refuses adoption.
template<gr::scheduler::ExecutionPolicy policy>
void stopHookFailure(bool rollback) {
    gr::SchedulerWrapper<gr::scheduler::Simple<policy>> scheduler;
    gr::SchedulerModel&                                 api = scheduler;
    gr::MsgPortIn                                       messages;
    require(api.asBlockModel()->msgOut->connect(messages));
    gr::Graph graph;
    auto&     source = graph.emplaceBlock<Source>();
    auto&     sink   = graph.emplaceBlock<Sink>();
    auto      stats  = sink.stats;
    require(graph.connect<"out", "in">(source, sink));
    api.setGraph(std::move(graph));
    require(api.start());
    await([&] { return stats->samples.load() > 10000; });

    std::shared_ptr<RefuseStop::Stats>         retired;
    std::vector<std::weak_ptr<gr::BlockModel>> blocks;
    auto                                       branch = api.compose([&](gr::CompositionEditor& edit) {
        auto& added = edit.emplaceBlock<RefuseStop>();
        retired     = added.stats;
        edit.connect(source, "out", added, "in");
        if (rollback) {
            auto& bad = edit.emplaceBlock<RefuseStart>();
            edit.connect(source, "out", bad, "in");
        }
        auto owned = api.asBlockModel()->graph()->blocks().subspan(2);
        blocks.assign(owned.begin(), owned.end());
    });
    if (rollback) {
        assert(!branch);
        assert(branch.error().message.find("deliberate adoption failure") != std::string::npos);
    } else {
        require(branch);
        await([&] { return retired->samples.load() > 10000; });
        require(api.removeBranch(branch->branch));
        assert(!api.removeBranch(branch->branch));
    }
    assert(retired->stops.load() == 1);
    assert(api.asBlockModel()->graph()->blocks().size() == 2);
    assert(!scheduler.blockRef().compositionInconsistent());
    assert(scheduler.blockRef().state() == gr::lifecycle::State::RUNNING);
    await([&] { return std::ranges::all_of(blocks, [](const auto& weak) { return weak.expired(); }); });
    const auto before = stats->samples.load();
    await([&] { return stats->samples.load() >= before + 10000; });
    assert(stats->errors.load() == 0);
    bool reported = false;
    await([&] {
        auto replies = messages.streamReader().get();
        for (const auto& reply : replies) {
            if (!reply.data && reply.data.error().message.find("deliberate retirement failure") != std::string::npos) {
                reported = true;
            }
        }
        assert(replies.consume(replies.size()));
        return reported;
    });
    api.stop();
}

template<gr::scheduler::ExecutionPolicy policy>
void quarantine(bool rollback) {
    gr::SchedulerWrapper<gr::scheduler::Simple<policy>> scheduler;
    gr::SchedulerModel&                                 api = scheduler;
    gr::Graph                                           graph;
    auto&                                               source = graph.emplaceBlock<Source>();
    auto&                                               sink   = graph.emplaceBlock<Sink>();
    require(graph.connect<"out", "in">(source, sink));
    auto stats = sink.stats;
    api.setGraph(std::move(graph));
    require(api.start());
    await([&] { return stats->samples.load() > 10000; });
    auto prior = api.compose([&](gr::CompositionEditor& edit) {
        auto& added       = edit.emplaceBlock<Sink>();
        added.stats->next = source.out.streamWriter().position();
        edit.connect(source, "out", added, "in");
    });
    require(prior);
    auto failed = api.compose([&](gr::CompositionEditor& edit) {
        auto& stop = edit.emplaceBlock<StopCallback>();
        edit.connect(source, "out", stop, "in");
        // Inject a structural retirement failure: the stop hook removes its own graph entry,
        // so retireCompositionBlock() cannot complete removeBlockByName().
        stop.onStop = [&, name = std::string(stop.unique_name)] { require(api.asBlockModel()->graph()->removeBlockByName(name)); };
        if (rollback) {
            auto& start = edit.emplaceBlock<RefuseStart>();
            edit.connect(source, "out", start, "in");
        }
    });
    if (rollback) {
        assert(!failed);
        assert(failed.error().message == "composition rollback failed; execution is inconsistent and must be stopped");
    } else {
        require(failed);
        auto removed = api.removeBranch(failed->branch);
        assert(!removed);
        assert(removed.error().message == "branch retirement failed; execution is inconsistent and must be stopped");
    }
    assert(scheduler.blockRef().compositionInconsistent());
    assert(scheduler.blockRef().state() == gr::lifecycle::State::ERROR);
    assert(!api.compose([](auto&) { assert(false); }));
    assert(!api.removeBranch(prior->branch));
    assert(!api.start());
    api.stop();
}
template<gr::scheduler::ExecutionPolicy policy>
void destruction() {
    using Base = gr::SchedulerWrapper<gr::scheduler::Simple<policy>>;
    struct Owned : Base {
        std::promise<void>* destroying = nullptr;
        ~Owned() override { destroying->set_value(); }
    };
    auto               scheduler = std::make_unique<Owned>();
    std::promise<void> destroying, entered, finish;
    scheduler->destroying = &destroying;
    auto      proceed     = finish.get_future().share();
    gr::Graph graph;
    auto&     source = graph.emplaceBlock<Source>();
    auto&     sink   = graph.emplaceBlock<Sink>();
    auto      stats  = sink.stats;
    require(graph.connect<"out", "in">(source, sink));
    scheduler->setGraph(std::move(graph));
    require(scheduler->start());
    await([&] { return stats->samples.load() > 10000; });
    auto* api     = static_cast<gr::SchedulerModel*>(scheduler.get());
    auto  editing = std::async(std::launch::async, [&] {
        return api->compose([&](gr::CompositionEditor& edit) {
            auto& added       = edit.emplaceBlock<Sink>();
            added.stats->next = source.out.streamWriter().position();
            edit.connect(source, "out", added, "in");
            entered.set_value();
            proceed.wait();
        });
    });
    entered.get_future().wait();
    auto deleting = std::async(std::launch::async, [&] { scheduler.reset(); });
    destroying.get_future().wait();
    finish.set_value();
    require(editing.get());
    deleting.get();
}
} // namespace qa_composition
int main() {
    using namespace qa_composition;
    try {
        qualify<gr::scheduler::ExecutionPolicy::singleThreaded>();
        qualify<gr::scheduler::ExecutionPolicy::multiThreaded>();
        retireAtStop<gr::scheduler::ExecutionPolicy::singleThreaded>();
        retireAtStop<gr::scheduler::ExecutionPolicy::multiThreaded>();
        destruction<gr::scheduler::ExecutionPolicy::singleThreaded>();
        destruction<gr::scheduler::ExecutionPolicy::multiThreaded>();
        drainDuringComposition<gr::scheduler::ExecutionPolicy::singleThreaded>();
        drainDuringComposition<gr::scheduler::ExecutionPolicy::multiThreaded>();
        stopHookFailure<gr::scheduler::ExecutionPolicy::singleThreaded>(false);
        stopHookFailure<gr::scheduler::ExecutionPolicy::multiThreaded>(false);
        stopHookFailure<gr::scheduler::ExecutionPolicy::singleThreaded>(true);
        stopHookFailure<gr::scheduler::ExecutionPolicy::multiThreaded>(true);
        quarantine<gr::scheduler::ExecutionPolicy::singleThreaded>(false);
        quarantine<gr::scheduler::ExecutionPolicy::multiThreaded>(false);
        quarantine<gr::scheduler::ExecutionPolicy::singleThreaded>(true);
        quarantine<gr::scheduler::ExecutionPolicy::multiThreaded>(true);
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
