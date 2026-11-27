#include <boost/ut.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <format>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <gnuradio-4.0/Block.hpp>
#include <gnuradio-4.0/BlockModel.hpp>
#include <gnuradio-4.0/Graph.hpp>
#include <gnuradio-4.0/Runtime.hpp>
#include <gnuradio-4.0/Scheduler.hpp>
#include <gnuradio-4.0/SchedulerModel.hpp>
#include <gnuradio-4.0/Settings.hpp>

#include "settings_utils.hpp"

namespace qa_settings_messages {

using namespace boost::ut;
using namespace std::chrono_literals;
using gr::property_map;
using gr::settings::Notification;
using gr::settings::Rejected;
using gr::settings::Rejection;
using gr::settings::Values;
using gr::settings::detail::FrameworkAccess;
using gr::testing::CerrCapture;
using gr::testing::textOf;
using gr::testing::waitUntil;
using Entry = gr::settings::Value;
using enum gr::scheduler::ExecutionPolicy;

// emits `n_samples` ones; with `n_samples` 0 it emits nothing and ends once `finish` is set
struct Ones : gr::Block<Ones> {
    gr::PortOut<float> out;

    struct Settings {
        gr::Size_t n_samples = 0U;

        GR_MAKE_REFLECTABLE(Settings, n_samples);
    } settings;

    GR_MAKE_REFLECTABLE(Ones, out, settings);

    std::size_t       sent = 0UZ;
    std::atomic<bool> finish{false};

    void start() { sent = 0UZ; }

    gr::work::Status processBulk(gr::OutputSpanLike auto& outSpan) {
        if (settings.n_samples == 0U) {
            outSpan.publish(0UZ);
            return finish.load() ? gr::work::Status::DONE : gr::work::Status::OK;
        }
        const std::size_t n = std::min(outSpan.size(), static_cast<std::size_t>(settings.n_samples) - sent);
        std::fill_n(outSpan.begin(), n, 1.f);
        outSpan.publish(n);
        sent += n;
        return sent >= settings.n_samples ? gr::work::Status::DONE : gr::work::Status::OK;
    }
};

// a gain whose device refuses a gain above `failAbove`, may report a gain and a code of its own at the start, and may
// fail its gain from its stop hook
struct Gain : gr::Block<Gain> {
    gr::PortIn<float>  in;
    gr::PortOut<float> out;

    struct Settings {
        float       gain = 1.f;
        std::string code{};

        GR_MAKE_REFLECTABLE(Settings, gain, code);
    } settings;

    GR_MAKE_REFLECTABLE(Gain, in, out, settings);

    float failAbove      = 1e9f;
    bool  roundsAtStart  = false;
    bool  failGainAtStop = false;

    void start() {
        if (roundsAtStart) {
            this->publish("gain", 2.5f, "the device rounds the gain");
            this->publish("code", std::string("B"));
        }
    }

    void stop() {
        if (failGainAtStop) {
            this->fail("gain", "the device lost its lock");
        }
    }

    // a failed gain takes back every key of the call
    void applySettings(const Settings& s, const Settings& old) {
        if (s.gain != old.gain && s.gain > failAbove) {
            this->fail(s.gain, "the device refuses this gain");
        }
    }

    [[nodiscard]] constexpr float processOne(float value) const noexcept { return value * settings.gain; }
};

struct Record : gr::Block<Record> {
    gr::PortIn<float> in;

    GR_MAKE_REFLECTABLE(Record, in);

    gr::work::Status processBulk(gr::InputSpanLike auto& inSpan) {
        std::ignore = inSpan.consume(inSpan.size());
        return gr::work::Status::OK;
    }
};

// a block without ports with four keys, each its own notification slot
struct Knobs : gr::Block<Knobs> {
    struct Settings {
        float a = 0.f;
        float b = 0.f;
        float c = 0.f;
        float d = 0.f;

        GR_MAKE_REFLECTABLE(Settings, a, b, c, d);
    } settings;

    GR_MAKE_REFLECTABLE(Knobs, settings);

    [[nodiscard]] gr::work::Status processBulk() { return gr::work::Status::OK; }
};

// a block without ports that sends its gains, then `refused`, as Sets without a request id to the block `target` names,
// from one work call once `armed` is set
struct Sender : gr::Block<Sender> {
    GR_MAKE_REFLECTABLE(Sender);

    gr::MsgPortOut     control;
    std::string        target{};
    std::vector<float> gains{};
    property_map       refused{};
    std::atomic<bool>  armed{false};
    std::atomic<bool>  sent{false};

    [[nodiscard]] gr::work::Status processBulk() {
        if (armed.load() && !sent.load()) {
            for (const float value : gains) {
                gr::sendMessage<gr::message::Command::Set>(control, target, gr::block::property::kSetting, property_map{{"gain", value}});
            }
            if (!refused.empty()) {
                gr::sendMessage<gr::message::Command::Set>(control, target, gr::block::property::kSetting, refused);
            }
            sent.store(true);
        }
        return gr::work::Status::OK;
    }
};

// a block whose work call fails the run
struct Faulty : gr::Block<Faulty> {
    GR_MAKE_REFLECTABLE(Faulty);

    [[nodiscard]] gr::work::Status processBulk() { return gr::work::Status::ERROR; }
};

// a silent source feeding a gain and a sink; the source ends once it is told to finish
struct Chain {
    gr::Graph flow;
    Ones&     source = flow.emplaceBlock<Ones>();
    Gain&     gain   = flow.emplaceBlock<Gain>();
    Record&   sink   = flow.emplaceBlock<Record>();

    Chain() {
        expect(fatal(flow.connect<"out", "in">(source, gain).has_value()));
        expect(fatal(flow.connect<"out", "in">(gain, sink).has_value()));
    }
};

// a test's ends of a scheduler's or a block's message ports, with every message read from it so far
struct Plane {
    gr::MsgPortOut           toScheduler;
    gr::MsgPortIn            fromScheduler;
    std::vector<gr::Message> received{};

    template<typename TTarget>
    explicit Plane(TTarget& target) {
        expect(fatal(toScheduler.connect(target.msgIn).has_value())) << "the request port did not connect";
        expect(fatal(target.msgOut.connect(fromScheduler).has_value())) << "the reply port did not connect";
    }

    void read() {
        auto messages = fromScheduler.streamReader().get();
        received.insert(received.end(), messages.begin(), messages.end());
        std::ignore = messages.consume(messages.size());
    }

    [[nodiscard]] std::size_t nReplies(std::string_view requestId) {
        read();
        return static_cast<std::size_t>(std::ranges::count_if(received, [requestId](const gr::Message& message) { return message.cmd == gr::message::Command::Final && message.clientRequestID == requestId; }));
    }

    [[nodiscard]] std::optional<gr::Message> reply(std::string_view requestId) {
        read();
        const auto found = std::ranges::find_if(received, [requestId](const gr::Message& message) { return message.cmd == gr::message::Command::Final && message.clientRequestID == requestId; });
        return found != received.end() ? std::optional(*found) : std::nullopt;
    }

    void set(std::string_view endpoint, std::string_view block, property_map changes, std::string_view requestId = {}) { gr::sendMessage<gr::message::Command::Set>(toScheduler, block, endpoint, std::move(changes), requestId); }
};

// the notifications a reader received, read from the delivering thread
struct Received {
    std::mutex                lock;
    std::vector<Notification> notifications{};

    [[nodiscard]] std::function<void(const Notification&)> reader() {
        return [this](const Notification& notification) {
            std::lock_guard guard(lock);
            notifications.push_back(notification);
        };
    }

    [[nodiscard]] std::size_t count(const std::function<bool(const Notification&)>& matches) {
        std::lock_guard guard(lock);
        return static_cast<std::size_t>(std::ranges::count_if(notifications, matches));
    }
};

[[nodiscard]] std::string textAt(const property_map& data, std::string_view key) {
    const auto it = data.find(key);
    return it != data.end() ? std::string(it->second.value_or(std::string_view{})) : std::string{};
}

// a scheduler whose idle workers park for up to 30 s, which no wake in these cases waits for, run by runAndWait() on a
// thread of its own
template<gr::scheduler::ExecutionPolicy kPolicy = multi_threaded_blocking>
struct ParkedRun {
    gr::scheduler::Simple<kPolicy> scheduler;
    Plane                          plane{scheduler};
    std::thread                    runner{};

    explicit ParkedRun(gr::Graph&& flow, gr::Size_t settingsTimeoutMs = 0U, gr::Size_t parkMs = 30000U) {
        const std::expected<Values, Rejected> parks = scheduler.set(property_map{{"timeout_ms", parkMs}, {"watchdog_timeout", gr::Size_t{30000U}}, {"settings_timeout_ms", settingsTimeoutMs}});
        expect(fatal(parks.has_value())) << "the scheduler refused its settings: " << textOf(parks);
        expect(fatal(scheduler.exchange(std::move(flow)).has_value())) << "the scheduler did not take the graph";
    }

    void start() {
        runner = std::thread([this] { std::ignore = scheduler.runAndWait(); });
        expect(fatal(waitUntil([this] { return FrameworkAccess::isRunning(gr::settings::detail::settingsOf(scheduler)); }))) << "the scheduler does not run";
    }

    ParkedRun(const ParkedRun&)            = delete;
    ParkedRun& operator=(const ParkedRun&) = delete;

    ~ParkedRun() { // a case that ends at a failed check still ends the run
        if (runner.joinable()) {
            std::ignore = scheduler.changeStateTo(gr::lifecycle::State::REQUESTED_STOP);
            runner.join();
        }
    }

    void stop(Ones& source) {
        source.finish = true;
        std::ignore   = scheduler.changeStateTo(gr::lifecycle::State::REQUESTED_STOP);
        runner.join();
    }
};

const suite<"a Set by message on the Settings endpoint"> settingsEndpointTests = [] {
    using gr::block::property::kEcho;
    using gr::block::property::kSetting;

    "a Set with a request id gets no reply, applies as one without, and its refusal is notified at error"_test = [] {
        Chain chain;
        chain.gain.failAbove = 10.f;
        const std::string gainName(chain.gain.unique_name);
        ParkedRun<>       run(std::move(chain.flow));
        Received          received;
        run.scheduler.installReader(received.reader());
        run.start();
        expect(fatal(waitUntil([&] { return FrameworkAccess::isRunning(gr::settings::detail::settingsOf(chain.gain)); })));

        run.plane.set(kSetting, gainName, {{"gain", 4.f}}, "applied");
        run.plane.set(kSetting, gainName, {{"no_such_key", 1.f}}, "refused");
        const auto naming = [](std::string_view key) { return [key](const Notification& notification) { return notification.level == gr::settings::NotificationLevel::error && notification.what == gr::settings::NotificationKind::failed && notification.reason.contains(key); }; };
        expect(fatal(waitUntil([&] { return received.count(naming("no_such_key")) == 1UZ; }))) << "the refusal of a Set with a request id reached no reader";
        expect(waitUntil([&] { return chain.gain.get<float>("gain").value_or(0.f) == 4.f; })) << "the Set with a request id did not apply";
        run.plane.set(kSetting, gainName, {{"gain", 20.f}});
        expect(waitUntil([&] { return received.count(naming("refuses")) == 1UZ; })) << "the failed apply of a Set reached no reader";
        // the block answers in order, so the reply to this Echo bounds any reply to the Sets before it
        run.plane.set(kEcho, gainName, {}, "after");
        expect(fatal(waitUntil([&] { return run.plane.reply("after").has_value(); }))) << "the Echo got no reply";
        expect(eq(run.plane.nReplies("applied"), 0UZ)) << "a Set with a request id got a reply";
        expect(eq(run.plane.nReplies("refused"), 0UZ)) << "a refused Set with a request id got a reply";
        run.stop(chain.source);
    };
};

const suite<"the messages between blocks and the runtime's Set"> sendTests = [] {
    "a Set another block sends without a request id applies at the receiver, the last to arrive winning, and a refusal is notified"_test = [] {
        Chain   chain;
        Sender& sender = chain.flow.emplaceBlock<Sender>();
        sender.target  = std::string(chain.gain.unique_name);
        sender.gains   = {2.f, 3.f};
        sender.refused = {{"no_such_key", 1.f}};
        ParkedRun<multi_threaded> run(std::move(chain.flow));
        Received                  received;
        run.scheduler.installReader(received.reader());
        expect(fatal(sender.control.connect(run.scheduler.msgIn).has_value())); // the scheduler passes what the sender sends on to its blocks
        run.start();
        expect(fatal(waitUntil([&] { return FrameworkAccess::isRunning(gr::settings::detail::settingsOf(chain.gain)); })));
        sender.armed = true;
        expect(waitUntil([&] { return sender.sent.load() && chain.gain.get<float>("gain").value_or(0.f) == 3.f; })) << "the receiver does not read the last gain sent";
        const auto refusal = [&chain](const Notification& notification) { return notification.level == gr::settings::NotificationLevel::error && notification.what == gr::settings::NotificationKind::failed && notification.block == chain.gain.unique_name && notification.reason.contains("no_such_key"); };
        expect(waitUntil([&] { return received.count(refusal) == 1UZ; })) << "the refused Set from a block reached no reader";
        run.stop(chain.source);
    };

    "a Set sent to a graph that does not run waits in the message queue and applies once the graph starts"_test = [] {
        auto                                                   gainBlock = std::make_shared<gr::BlockWrapper<Gain>>();
        Gain&                                                  gain      = gainBlock->blockRef();
        gr::RuntimeGraph                                       graph;
        const std::expected<gr::BlockHandle, gr::RuntimeError> source = graph.add(std::make_shared<gr::BlockWrapper<Ones>>());
        const std::expected<gr::BlockHandle, gr::RuntimeError> handle = graph.add(gainBlock);
        const std::expected<gr::BlockHandle, gr::RuntimeError> sink   = graph.add(std::make_shared<gr::BlockWrapper<Record>>());
        expect(fatal(source.has_value() && handle.has_value() && sink.has_value()));
        expect(fatal(graph.connect(*source, "out", *handle, "in").has_value()));
        expect(fatal(graph.connect(*handle, "out", *sink, "in").has_value()));
        std::expected<gr::Runtime, gr::RuntimeError> runtime = gr::Runtime::create(std::move(graph));
        expect(fatal(runtime.has_value()));

        expect(fatal(runtime->send(gr::RuntimeCommand::Set, handle->uniqueName(), gr::Runtime::kSettingsEndpoint, {{"gain", 4.f}}).has_value()));
        expect(eq(handle->get<float>("gain").value_or(0.f), 1.f)) << "the Set was taken before the graph started";
        expect(fatal(runtime->start().has_value()));
        expect(waitUntil([&] { return handle->get<float>("gain").value_or(0.f) == 4.f; })) << "the Set that waited did not apply after the start";
        runtime->stop();
        expect(eq(gain.settings.gain, 4.f));
    };
};

const suite<"the scheduler's subscriptions"> subscriptionTests = [] {
    "each endpoint of a scheduler holds at most kMaxSubscribersPerProperty clients, and its lifecycle notifies each"_test = [] {
        using enum gr::message::Command;
        constexpr std::size_t   kCap = gr::message::kMaxSubscribersPerProperty;
        gr::scheduler::Simple<> scheduler;
        gr::MsgPortOut          toScheduler;
        gr::MsgPortIn           fromScheduler;
        expect(fatal(scheduler.msgOut.connect(fromScheduler).has_value()));
        expect(fatal(toScheduler.connect(scheduler.msgIn).has_value()));
        const auto take = [&fromScheduler] {
            gr::ReaderSpanLike auto  span = fromScheduler.streamReader().get(fromScheduler.streamReader().available());
            std::vector<gr::Message> taken(span.begin(), span.end());
            std::ignore = span.consume(span.size());
            return taken;
        };
        const auto clientID = [](std::size_t i) { return std::format("client-{}", i); };
        for (const std::string_view endpoint : {std::string_view(gr::block::property::kLifeCycleState), std::string_view(gr::block::property::kSettingsNotification)}) {
            for (std::size_t i = 0UZ; i <= kCap; ++i) {
                gr::sendMessage<Subscribe>(toScheduler, "", endpoint, property_map{}, clientID(i));
            }
            scheduler.processScheduledMessages();
            const std::vector<gr::Message> replies = take();
            expect(fatal(eq(replies.size(), 1UZ))) << endpoint;
            expect(!replies.front().data.has_value() && replies.front().clientRequestID == clientID(kCap)) << "the client past the cap is refused on" << endpoint;
        }
        gr::sendMessage<Unsubscribe>(toScheduler, "", gr::block::property::kLifeCycleState, property_map{}, clientID(1UZ));
        gr::sendMessage<Subscribe>(toScheduler, "", gr::block::property::kLifeCycleState, property_map{}, clientID(kCap));
        scheduler.processScheduledMessages();
        expect(take().empty()) << "a freed slot takes a new client";

        expect(fatal(scheduler.changeStateTo(gr::lifecycle::State::INITIALISED).has_value()));
        const std::vector<gr::Message> notices = take();
        expect(eq(std::ranges::count_if(notices, [](const gr::Message& notice) { return notice.endpoint == gr::block::property::kLifeCycleState; }), static_cast<std::ptrdiff_t>(kCap))) << "one notice per client";
        expect(std::ranges::any_of(notices, [&](const gr::Message& notice) { return notice.clientRequestID == clientID(kCap); })) << "the freed slot went to the new client";
        expect(std::ranges::none_of(notices, [&](const gr::Message& notice) { return notice.clientRequestID == clientID(1UZ); }));
    };
};

const suite<"the delivery toward the event stream and the parent"> deliveryTests = [] {
    "the event stream holds at most 16 notifications unread, and carries every key's latest at later passes"_test = [] {
        CerrCapture         captured;
        gr::RuntimeGraph    graph;
        std::vector<Knobs*> knobs;
        for (std::size_t i = 0UZ; i < 10UZ; ++i) {
            auto block = std::make_shared<gr::BlockWrapper<Knobs>>();
            knobs.push_back(std::addressof(block->blockRef()));
            expect(fatal(graph.add(block).has_value()));
        }
        std::expected<gr::Runtime, gr::RuntimeError> runtime = gr::Runtime::create(std::move(graph));
        expect(fatal(runtime.has_value()));
        expect(fatal(runtime->send(gr::RuntimeCommand::Subscribe, "", gr::Runtime::kSettingsNotificationEndpoint, {}, "host").has_value()));
        // raised before the run, all 40 wait in the queue for the first message pass, which delivers them at once
        for (Knobs* block : knobs) {
            for (const char* key : {"a", "b", "c", "d"}) {
                gr::settings::detail::FrameworkAccess::fail(gr::settings::detail::settingsOf(*block), key, "the device lost its lock");
            }
        }
        expect(fatal(runtime->start().has_value()));

        std::vector<gr::RuntimeEvent> notifications;
        const auto                    poll = [&] {
            std::size_t nNew = 0UZ;
            for (gr::RuntimeEvent& event : runtime->pollEvents()) {
                if (event.endpoint == gr::Runtime::kSettingsNotificationEndpoint) {
                    notifications.push_back(std::move(event));
                    ++nNew;
                }
            }
            return nNew;
        };
        std::size_t firstPoll = 0UZ;
        expect(fatal(waitUntil([&] { return (firstPoll = poll()) > 0UZ; }))) << "no notification reached the event stream";
        expect(eq(firstPoll, 16UZ)) << "the event stream held more or fewer than 16 unread";
        expect(fatal(waitUntil([&] {
            expect(le(poll(), 16UZ)) << "a poll read more than 16";
            return notifications.size() >= 40UZ;
        }))) << std::format("{} of the 40 notifications arrived", notifications.size());
        runtime->stop();

        std::set<std::string> slots;
        for (const gr::RuntimeEvent& event : notifications) {
            slots.insert(std::format("{}.{}", textAt(event.data, "block"), textAt(event.data, "key")));
            expect(eq(event.source, textAt(event.data, "block")));
            expect(eq(textAt(event.data, "what"), std::string("failed")));
            expect(eq(textAt(event.data, "reason"), std::string("the device lost its lock")));
        }
        expect(eq(slots.size(), 40UZ)) << "a key's notification arrived twice or not at all";
        expect(!captured.str().contains("lost its lock")) << "the default reader printed beside the event stream";
    };

    "a Runtime's host that does not subscribe reads a failure after the start on stderr, one that subscribes reads it as an event alone"_test = [] {
        for (const bool subscribes : {false, true}) {
            CerrCapture                                            captured;
            auto                                                   gainBlock   = std::make_shared<gr::BlockWrapper<Gain>>();
            auto                                                   sourceBlock = std::make_shared<gr::BlockWrapper<Ones>>();
            gr::RuntimeGraph                                       graph;
            const std::expected<gr::BlockHandle, gr::RuntimeError> source = graph.add(sourceBlock);
            const std::expected<gr::BlockHandle, gr::RuntimeError> handle = graph.add(gainBlock);
            const std::expected<gr::BlockHandle, gr::RuntimeError> sink   = graph.add(std::make_shared<gr::BlockWrapper<Record>>());
            expect(fatal(source.has_value() && handle.has_value() && sink.has_value()));
            expect(fatal(graph.connect(*source, "out", *handle, "in").has_value()));
            expect(fatal(graph.connect(*handle, "out", *sink, "in").has_value()));
            std::expected<gr::Runtime, gr::RuntimeError> runtime = gr::Runtime::create(std::move(graph));
            expect(fatal(runtime.has_value()));
            if (subscribes) {
                expect(fatal(runtime->send(gr::RuntimeCommand::Subscribe, "", gr::Runtime::kSettingsNotificationEndpoint, {}, "host").has_value()));
            }
            expect(fatal(runtime->start().has_value()));

            gr::settings::detail::FrameworkAccess::fail(gr::settings::detail::settingsOf(gainBlock->blockRef()), "gain", "the device lost its lock"); // the device's thread, while the graph runs
            std::vector<gr::RuntimeEvent> notifications;
            const auto                    poll = [&] {
                for (gr::RuntimeEvent& event : runtime->pollEvents()) {
                    if (event.endpoint == gr::Runtime::kSettingsNotificationEndpoint) {
                        notifications.push_back(std::move(event));
                    }
                }
                return !notifications.empty();
            };
            if (subscribes) {
                expect(fatal(waitUntil(poll))) << "the subscribed host read no event";
            }
            sourceBlock->blockRef().finish = true;
            runtime->stop(); // the stop delivers what the run raised
            std::ignore = poll();

            expect(eq(notifications.size(), subscribes ? 1UZ : 0UZ)) << std::format("subscribed: {}", subscribes);
            expect(eq(captured.str().contains("lost its lock"), !subscribes)) << std::format("subscribed: {}, printed: {}", subscribes, captured.str());
            if (!notifications.empty()) {
                expect(eq(textAt(notifications.front().data, "key"), std::string("gain")));
                expect(eq(textAt(notifications.front().data, "level"), std::string("error")));
            }
        }
    };

    "an event that waits for room in the event stream is replaced by the next of its block, key and level, with the count"_test = [] {
        gr::Graph           flow;
        std::vector<Knobs*> knobs;
        for (std::size_t i = 0UZ; i < 5UZ; ++i) {
            knobs.push_back(std::addressof(flow.emplaceBlock<Knobs>()));
        }
        ParkedRun<multi_threaded> run(std::move(flow));
        Received                  received; // a reader the delivery calls before it writes the event stream
        run.scheduler.installReader(received.reader());
        // raised before the run, the 20 reach the first delivery, which leaves at least the last 4 in the event queue
        for (Knobs* block : knobs) {
            for (const char* key : {"a", "b", "c", "d"}) {
                gr::settings::detail::FrameworkAccess::fail(gr::settings::detail::settingsOf(*block), key, "the device lost its lock");
            }
        }
        run.start();
        const auto all = [](const Notification&) { return true; };
        expect(fatal(waitUntil([&] { return received.count(all) >= 20UZ; }))) << "the start's delivery reached the reader short";

        // the event stream holds what the host has not read, so the next delivery only replaces the waiting event
        const std::string lastBlock(knobs.back()->unique_name);
        gr::settings::detail::FrameworkAccess::fail(gr::settings::detail::settingsOf(*knobs.back()), "d", "the device lost its lock again");
        expect(fatal(waitUntil([&] { return received.count([](const Notification& notification) { return notification.reason.contains("again"); }) == 1UZ; })));
        run.plane.read();
        gr::settings::detail::FrameworkAccess::fail(gr::settings::detail::settingsOf(*knobs.front()), "a", "a later failure"); // a delivery once the host has read
        const auto lastKeyEvents = [&] {
            std::vector<gr::Message> events;
            std::ranges::copy_if(run.plane.received, std::back_inserter(events), [&lastBlock](const gr::Message& message) { return message.cmd == gr::message::Command::Notify && message.endpoint == gr::block::property::kSettingsNotification && message.data.has_value() && textAt(*message.data, "block") == lastBlock && textAt(*message.data, "key") == "d"; });
            return events;
        };
        expect(fatal(waitUntil([&] {
            run.plane.read();
            return std::ranges::any_of(run.plane.received, [](const gr::Message& message) { return message.data.has_value() && textAt(*message.data, "reason") == "a later failure"; });
        }))) << "the delivery after the read sent nothing";
        const std::vector<gr::Message> events = lastKeyEvents();
        expect(fatal(eq(events.size(), 1UZ))) << "the waiting event was not replaced";
        expect(eq(textAt(*events.front().data, "reason"), std::string("the device lost its lock again")));
        expect(eq(events.front().data->at("replaced").value_or(std::uint64_t{0U}), std::uint64_t{1U})) << "the event does not count the one it replaced";
    };

    "a nested scheduler stopped while its parent does not run delivers its stop's notification to the parent's reader"_test = [] {
        Chain chain;
        auto  inner = std::make_shared<gr::SchedulerWrapper<gr::scheduler::Simple<multi_threaded>>>();
        inner->setGraph(std::move(chain.flow));
        gr::Graph outer;
        outer.addBlock(gr::SchedulerModel::asBlockModelPtr(inner));
        gr::scheduler::Simple<multi_threaded> top; // never started
        expect(fatal(top.exchange(std::move(outer)).has_value()));
        Received received;
        top.installReader(received.reader());

        expect(fatal(inner->start().has_value()));
        expect(fatal(waitUntil([&] { return FrameworkAccess::isRunning(gr::settings::detail::settingsOf(chain.gain)); })));
        chain.gain.failGainAtStop = true;
        chain.source.finish       = true;
        inner->stop();
        expect(eq(received.count([](const Notification& notification) { return notification.key == "gain" && notification.reason == "the device lost its lock"; }), 1UZ)) << "the nested stop's failure did not reach the parent's reader";
    };
};

const suite<"the runtime's runs report their settings"> runTests = [] {
    "runAndWait() notifies each start value in force that differs from the value given, and a start that returns them does not"_test = [] {
        for (const bool returned : {false, true}) {
            gr::Graph flow;
            auto&     source = flow.emplaceBlock<Ones>({{"n_samples", gr::Size_t{64U}}});
            auto&     gain   = flow.emplaceBlock<Gain>();
            auto&     sink   = flow.emplaceBlock<Record>();
            expect(fatal(flow.connect<"out", "in">(source, gain).has_value()));
            expect(fatal(flow.connect<"out", "in">(gain, sink).has_value()));
            gain.roundsAtStart = true;
            gr::scheduler::Simple<> scheduler;
            expect(fatal(scheduler.exchange(std::move(flow)).has_value()));
            expect(fatal(scheduler.set("notification_level", std::string("info")).has_value()));
            expect(fatal(gain.set(property_map{{"gain", 3.f}, {"code", std::string("A")}}).has_value()));
            Received received;
            scheduler.installReader(received.reader());
            FrameworkAccess::returnStartValues(scheduler, returned);
            expect(fatal(scheduler.runAndWait().has_value()));

            const auto substituted = [&received](std::string_view key, std::string_view reason) { return received.count([key, reason](const Notification& notification) { return notification.what == gr::settings::NotificationKind::changed && notification.level == gr::settings::NotificationLevel::info && notification.key == key && notification.reason == reason; }); };
            // the start hook's publish of the gain is itself a change with the same reason, so the code tells the two paths apart
            expect(ge(substituted("gain", "the device rounds the gain"), 1UZ)) << std::format("start values returned: {}", returned);
            expect(eq(substituted("code", "the value in force differs"), returned ? 0UZ : 1UZ)) << std::format("start values returned: {}", returned);
        }
    };

    // the run's error names the faulting block whatever the message plane sends at the run's end
    "runAndWait() returns an error naming the block that faulted, and success for a run that ends cleanly"_test = [] {
        gr::Graph               faulting;
        const Faulty&           faulty = faulting.emplaceBlock<Faulty>();
        gr::scheduler::Simple<> failing;
        expect(fatal(failing.exchange(std::move(faulting)).has_value()));
        const std::expected<void, gr::Error> failed = failing.runAndWait();
        expect(fatal(!failed.has_value())) << "a faulting run returned success";
        expect(failed.error().message.contains(std::string(faulty.unique_name))) << failed.error().message;

        gr::Graph flow;
        auto&     source = flow.emplaceBlock<Ones>({{"n_samples", gr::Size_t{64U}}});
        auto&     sink   = flow.emplaceBlock<Record>();
        expect(fatal(flow.connect<"out", "in">(source, sink).has_value()));
        gr::scheduler::Simple<> clean;
        expect(fatal(clean.exchange(std::move(flow)).has_value()));
        const std::expected<void, gr::Error> ran = clean.runAndWait();
        expect(ran.has_value()) << (ran ? std::string{} : ran.error().message);
    };

    "Runtime::resume() returns the resume's values, a failed apply among them, and the graph runs on"_test = [] {
        auto  gainBlock   = std::make_shared<gr::BlockWrapper<Gain>>();
        auto  sourceBlock = std::make_shared<gr::BlockWrapper<Ones>>();
        Gain& gain        = gainBlock->blockRef();
        gain.failAbove    = 10.f;
        gr::RuntimeGraph                                       graph;
        const std::expected<gr::BlockHandle, gr::RuntimeError> source = graph.add(sourceBlock);
        const std::expected<gr::BlockHandle, gr::RuntimeError> handle = graph.add(gainBlock);
        const std::expected<gr::BlockHandle, gr::RuntimeError> sink   = graph.add(std::make_shared<gr::BlockWrapper<Record>>());
        expect(fatal(source.has_value() && handle.has_value() && sink.has_value()));
        expect(fatal(graph.connect(*source, "out", *handle, "in").has_value()));
        expect(fatal(graph.connect(*handle, "out", *sink, "in").has_value()));
        std::expected<gr::Runtime, gr::RuntimeError> runtime = gr::Runtime::create(std::move(graph), "gr::scheduler::Simple<multi_threaded>");
        expect(fatal(runtime.has_value())) << (runtime ? std::string{} : runtime.error().message);
        expect(fatal(runtime->start().has_value()));

        const std::expected<Values, Rejected> notPaused = runtime->resume();
        expect(fatal(!notPaused.has_value())) << "a running graph resumed";
        expect(fatal(eq(notPaused.error().size(), 1UZ)));
        expect(eq(notPaused.error().front().key, runtime->scheduler().name()));

        expect(fatal(runtime->pause().has_value()));
        expect(runtime->state() == gr::Runtime::State::Paused);
        expect(fatal(gain.set(property_map{{"gain", 20.f}, {"code", std::string("D")}}, gr::settings::noWait).has_value()));
        const std::expected<Values, Rejected> resumed = runtime->resume();
        expect(fatal(resumed.has_value())) << textOf(resumed);
        const std::string gainKey = std::format("{}.gain", handle->name());
        expect(fatal(resumed->contains(gainKey))) << textOf(resumed);
        expect(resumed->at(gainKey).state == Entry::State::failed);
        expect(resumed->at(gainKey).reason.contains("refuses")) << resumed->at(gainKey).reason;
        const std::string codeKey = std::format("{}.code", handle->name());
        expect(fatal(resumed->contains(codeKey))) << "the failed gain took back the code of its batch";
        expect(resumed->at(codeKey).state == Entry::State::failed);
        expect(resumed->at(codeKey).reason.starts_with("gain: ")) << resumed->at(codeKey).reason;
        expect(runtime->state() == gr::Runtime::State::Running);
        sourceBlock->blockRef().finish = true;
        runtime->stop();
    };
};

} // namespace qa_settings_messages
