#include <gnuradio-4.0/TriggerMatcher.hpp>

#include <iostream>

std::ostream& operator<<(std::ostream& os, const gr::trigger::MatchResult& result) { return os << std::format("{}", result); }

#include <boost/ut.hpp>

const boost::ut::suite<"BasicTriggerNameCtxMatcher"> triggerTest = [] {
    using namespace boost::ut;
    using namespace gr;

    "trigger parser"_test = [] {
        using namespace std::string_literals;
        std::string triggerName;
        bool        triggerNameEnds = false;
        std::string triggerCtx;
        bool        triggerCtxEnds = false;

        "full <trigger name>/<ctx>"_test = [&] {
            expect(nothrow([&] { trigger::detail::parse("alarm/kitchen", triggerName, triggerNameEnds, triggerCtx, triggerCtxEnds); }));
            expect(eq(triggerName, "alarm"s));
            expect(eq(triggerCtx, "kitchen"s));
            expect(!triggerNameEnds);
            expect(!triggerCtxEnds);

            expect(nothrow([&] { trigger::detail::parse("^alarm/kitchen", triggerName, triggerNameEnds, triggerCtx, triggerCtxEnds); }));
            expect(eq(triggerName, "alarm"s));
            expect(eq(triggerCtx, "kitchen"s));
            expect(triggerNameEnds);
            expect(!triggerCtxEnds);

            expect(nothrow([&] { trigger::detail::parse("alarm/^kitchen", triggerName, triggerNameEnds, triggerCtx, triggerCtxEnds); }));
            expect(eq(triggerName, "alarm"s));
            expect(eq(triggerCtx, "kitchen"s));
            expect(!triggerNameEnds);
            expect(triggerCtxEnds);

            expect(nothrow([&] { trigger::detail::parse("^alarm/^kitchen", triggerName, triggerNameEnds, triggerCtx, triggerCtxEnds); }));
            expect(eq(triggerName, "alarm"s));
            expect(eq(triggerCtx, "kitchen"s));
            expect(triggerNameEnds);
            expect(triggerCtxEnds);
        };

        "<trigger name> only"_test = [&] {
            expect(nothrow([&] { trigger::detail::parse("alarm", triggerName, triggerNameEnds, triggerCtx, triggerCtxEnds); }));
            expect(eq(triggerName, "alarm"s));
            expect(eq(triggerCtx, ""s));
        };

        "/<ctx> only"_test = [&] {
            expect(nothrow([&] { trigger::detail::parse("/kitchen", triggerName, triggerNameEnds, triggerCtx, triggerCtxEnds); }));
            expect(eq(triggerName, ""s));
            expect(eq(triggerCtx, "kitchen"s));
        };

        "extraneous separator <trigger name>/<ctx>/<..>"_test = [&] {
            expect(throws([&] { trigger::detail::parse("alarm/kitchen/cabinet", triggerName, triggerNameEnds, triggerCtx, triggerCtxEnds); })); //
        };
    };

    "BasicTriggerNameCtxMatcher Tests"_test = [] {
        using namespace std::string_literals;
        using enum gr::trigger::MatchResult;
        constexpr auto createTag = [](std::string triggerName, std::string cxt) noexcept { return Tag(0, {{tag::TRIGGER_NAME.shortKey(), triggerName}, {tag::CONTEXT.shortKey(), cxt}, {tag::TRIGGER_META_INFO.shortKey(), property_map{}}}); };

        "trigger on room1-room3 (exclusive)"_test = [&] {
            auto&          matcher = trigger::BasicTriggerNameCtxMatcher::filter;
            constexpr auto filter  = "[alarm/room1, alarm/room3]";
            property_map   state;

            expect(nothrow([&state] { expect(eq(matcher(filter, Tag{}, state), Ignore)); }));
            expect(!state.at("isSingleTrigger").value_or(true));

            expect(eq(matcher(filter, createTag("alarm", "room1"), state), Matching));
            expect(eq(matcher(filter, createTag("alarm", "room1"), state), Ignore));
            expect(eq(matcher(filter, createTag("alarm", "room2"), state), Ignore));
            expect(eq(matcher(filter, createTag("info", "room2"), state), Ignore));
            expect(eq(matcher(filter, createTag("alarm", "room3"), state), NotMatching));
            expect(eq(matcher(filter, createTag("alarm", "room4"), state), Ignore));
            expect(eq(matcher(filter, createTag("alarm", "room1"), state), Matching));
            expect(eq(matcher(filter, createTag("alarm", "room1"), state), Ignore));
            expect(eq(matcher(filter, createTag("info", "room2"), state), Ignore));
            expect(eq(matcher(filter, createTag("alarm", "room2"), state), Ignore));
            expect(eq(matcher(filter, createTag("alarm", "room3"), state), NotMatching));
            expect(eq(matcher(filter, createTag("alarm", "room4"), state), Ignore));

            expect(nothrow([&state] { trigger::BasicTriggerNameCtxMatcher::reset(state); })) << "reset matcher for next scenario";
        };

        "trigger on room1-^room3 (inclusive)"_test = [&] {
            auto&          matcher = trigger::BasicTriggerNameCtxMatcher::filter;
            constexpr auto filter  = "[alarm/room1, alarm/^room3]";
            property_map   state;

            expect(nothrow([&state] { expect(eq(matcher(filter, Tag{}, state), Ignore)); }));
            expect(!state.at("isSingleTrigger").value_or(true));

            expect(eq(matcher(filter, createTag("alarm", "room1"), state), Matching));
            expect(eq(matcher(filter, createTag("alarm", "room1"), state), Ignore));
            expect(eq(matcher(filter, createTag("alarm", "room2"), state), Ignore));
            expect(eq(matcher(filter, createTag("alarm", "room3"), state), Ignore));
            expect(eq(matcher(filter, createTag("alarm", "room4"), state), NotMatching));
            expect(eq(matcher(filter, createTag("alarm", "room1"), state), Matching));
            expect(eq(matcher(filter, createTag("alarm", "room1"), state), Ignore));
            expect(eq(matcher(filter, createTag("alarm", "room2"), state), Ignore));
            expect(eq(matcher(filter, createTag("alarm", "room3"), state), Ignore));
            expect(eq(matcher(filter, createTag("alarm", "room4"), state), NotMatching));
        };

        "trigger on room1-^room3 (inclusive)"_test = [&] {
            auto&          matcher = trigger::BasicTriggerNameCtxMatcher::filter;
            constexpr auto filter  = "[alarm/^room1, alarm/^room3]"; // implicitly resets
            property_map   state;

            expect(nothrow([&state] { expect(eq(matcher(filter, Tag{}, state), Ignore)); }));
            expect(!state.at("isSingleTrigger").value_or(true));

            expect(eq(matcher(filter, createTag("alarm", "room1"), state), Ignore));
            expect(eq(matcher(filter, createTag("info", "room2"), state), Matching));
            expect(eq(matcher(filter, createTag("alarm", "room3"), state), Ignore));
            expect(eq(matcher(filter, createTag("alarm", "room4"), state), NotMatching));
            expect(eq(matcher(filter, createTag("alarm", "room1"), state), Ignore));
            expect(eq(matcher(filter, createTag("alarm", "room2"), state), Matching));
            expect(eq(matcher(filter, createTag("alarm", "room3"), state), Ignore));
            expect(eq(matcher(filter, createTag("alarm", "room4"), state), NotMatching));
        };

        "trigger on ^alarm/room1 to alarm/room3"_test = [&] {
            auto&          matcher = trigger::BasicTriggerNameCtxMatcher::filter;
            constexpr auto filter  = "[^alarm/room1, alarm/room3]";
            property_map   state;

            expect(nothrow([&state] { expect(eq(matcher(filter, Tag{}, state), Ignore)); }));
            expect(!state.at("isSingleTrigger").value_or(true));

            expect(eq(matcher(filter, createTag("alarm", "room1"), state), Ignore)); // skipped due to ^alarm
            expect(eq(matcher(filter, createTag("other", "room1"), state), Matching));
            expect(eq(matcher(filter, createTag("alarm", "room1"), state), Ignore));
            expect(eq(matcher(filter, createTag("alarm", "room2"), state), Ignore));
            expect(eq(matcher(filter, createTag("other", "room1"), state), Ignore));
            expect(eq(matcher(filter, createTag("alarm", "room3"), state), NotMatching));
            expect(eq(matcher(filter, createTag("alarm", "room1"), state), Ignore));
            expect(eq(matcher(filter, createTag("other", "room1"), state), Matching));
            expect(eq(matcher(filter, createTag("alarm", "room3"), state), NotMatching));
        };

        "trigger with ^alarm/^room1 to ^alarm/room3"_test = [&] {
            auto&          matcher = trigger::BasicTriggerNameCtxMatcher::filter;
            constexpr auto filter  = "[^alarm/^room1, ^alarm/room3]";
            property_map   state;

            expect(nothrow([&state] { expect(eq(matcher(filter, Tag{}, state), Ignore)); }));
            expect(!state.at("isSingleTrigger").value_or(true));

            expect(eq(matcher(filter, createTag("alarm", "room1"), state), Ignore)); // skipped due to ^alarm/^room1
            expect(eq(matcher(filter, createTag("other", "room2"), state), Matching));
            expect(eq(matcher(filter, createTag("other", "room1"), state), Ignore));
            expect(eq(matcher(filter, createTag("alarm", "room2"), state), Ignore));
            expect(eq(matcher(filter, createTag("other", "room3"), state), Ignore));
            expect(eq(matcher(filter, createTag("alarm", "room3"), state), Ignore)); // skipped due to ^alarm/^room3
            expect(eq(matcher(filter, createTag("other", "room4"), state), NotMatching));
            expect(eq(matcher(filter, createTag("alarm", "room1"), state), Ignore)); // skipped due to ^alarm/^room1
            expect(eq(matcher(filter, createTag("other", "room2"), state), Matching));
            expect(eq(matcher(filter, createTag("alarm", "room3"), state), Ignore)); // skipped due to ^alarm/^room3
            expect(eq(matcher(filter, createTag("other", "room4"), state), NotMatching));
        };

        "trigger with alarm/^room1 to alarm/^room3"_test = [&] {
            auto&          matcher = trigger::BasicTriggerNameCtxMatcher::filter;
            constexpr auto filter  = "[alarm/^room1, alarm/^room3]";
            property_map   state;

            expect(nothrow([&state] { expect(eq(matcher(filter, Tag{}, state), Ignore)); }));
            expect(!state.at("isSingleTrigger").value_or(true));

            expect(eq(matcher(filter, createTag("alarm", "room1"), state), Ignore));
            expect(eq(matcher(filter, createTag("alarm", "room2"), state), Matching));
            expect(eq(matcher(filter, createTag("alarm", "room1"), state), Ignore));
            expect(eq(matcher(filter, createTag("alarm", "room3"), state), Ignore));
            expect(eq(matcher(filter, createTag("alarm", "room4"), state), NotMatching));
            expect(eq(matcher(filter, createTag("alarm", "room1"), state), Ignore));
            expect(eq(matcher(filter, createTag("alarm", "room2"), state), Matching));
            expect(eq(matcher(filter, createTag("alarm", "room3"), state), Ignore));
            expect(eq(matcher(filter, createTag("alarm", "room4"), state), NotMatching));
        };

        "mixed trigger conditions"_test = [&] {
            auto&          matcher = trigger::BasicTriggerNameCtxMatcher::filter;
            constexpr auto filter  = "[^alarm/room1, alarm/room3]";
            property_map   state;

            expect(nothrow([&state] { expect(eq(matcher(filter, Tag{}, state), Ignore)); }));
            expect(!state.at("isSingleTrigger").value_or(true));

            expect(eq(matcher(filter, createTag("alarm", "room1"), state), Ignore));
            expect(eq(matcher(filter, createTag("other", "room1"), state), Matching));
            expect(eq(matcher(filter, createTag("alarm", "room1"), state), Ignore));
            expect(eq(matcher(filter, createTag("alarm", "room2"), state), Ignore));
            expect(eq(matcher(filter, createTag("alarm", "room3"), state), NotMatching));
            expect(eq(matcher(filter, createTag("alarm", "room1"), state), Ignore));
            expect(eq(matcher(filter, createTag("other", "room1"), state), Matching));
            expect(eq(matcher(filter, createTag("alarm", "room3"), state), NotMatching));
        };

        "single trigger 1"_test = [&] {
            auto&          matcher = trigger::BasicTriggerNameCtxMatcher::filter;
            constexpr auto filter  = "[alarm/room1]";
            property_map   state;

            expect(nothrow([&state] { expect(eq(matcher(filter, Tag{}, state), Ignore)); }));
            expect(state.at("isSingleTrigger").value_or(true));

            expect(eq(matcher(filter, createTag("alarm", "room1"), state), Matching));
            expect(eq(matcher(filter, createTag("alarm", "room1"), state), Matching));
            expect(eq(matcher(filter, createTag("other", "room1"), state), Ignore));
            expect(eq(matcher(filter, createTag("alarm", "room1"), state), Matching));
            expect(eq(matcher(filter, createTag("alarm", "room1"), state), Matching));
        };

        "single trigger 2"_test = [&] {
            auto&          matcher = trigger::BasicTriggerNameCtxMatcher::filter;
            constexpr auto filter  = "[, alarm/room1]"; // note: extra ',' separator
            property_map   state;

            expect(nothrow([&state] { expect(eq(matcher(filter, Tag{}, state), Ignore)); }));
            expect(state.at("isSingleTrigger").value_or(true));

            expect(eq(matcher(filter, createTag("alarm", "room1"), state), Matching));
            expect(eq(matcher(filter, createTag("alarm", "room1"), state), Matching));
            expect(eq(matcher(filter, createTag("other", "room1"), state), Ignore));
            expect(eq(matcher(filter, createTag("alarm", "room1"), state), Matching));
            expect(eq(matcher(filter, createTag("alarm", "room1"), state), Matching));
            expect(trigger::BasicTriggerNameCtxMatcher::isSingleTrigger(state));
        };

        "single trigger 3"_test = [&] {
            auto&          matcher = trigger::BasicTriggerNameCtxMatcher::filter;
            constexpr auto filter  = "[alarm/room1, alarm/room1]";
            property_map   state;

            expect(nothrow([&state] { expect(eq(matcher(filter, Tag{}, state), Ignore)); }));
            expect(state.at("isSingleTrigger").value_or(true));

            expect(eq(matcher(filter, createTag("alarm", "room1"), state), Matching));
            expect(eq(matcher(filter, createTag("alarm", "room1"), state), Matching));
            expect(eq(matcher(filter, createTag("other", "room1"), state), Ignore));
            expect(eq(matcher(filter, createTag("alarm", "room1"), state), Matching));
            expect(eq(matcher(filter, createTag("alarm", "room1"), state), Matching));
        };
    };

    // the state map is the caller's, so a value of the wrong type in it is unusable input rather
    // than a broken invariant: the matcher reports the state as unusable instead of ending the
    // process, which is what a terminating pointer read would do
    "a state map carrying the wrong types is refused, not fatal"_test = [] {
        using enum gr::trigger::MatchResult;
        constexpr auto filter = "[alarm/room1, alarm/room3]";

        // the criteria already stored, so the matcher keeps the values below instead of rebuilding them
        property_map state{{"filter", std::string(filter)}, {"startDefined", 42}, {"stopDefined", 42}, {"triggerActive", 42}, //
            {"startTriggerName", std::string("alarm")}, {"startCtx", std::string("room1")},                                   //
            {"stopTriggerName", std::string("alarm")}, {"stopCtx", std::string("room3")},                                     //
            {"startTriggerNameEnds", 42}, {"startCtxEnds", 42}, {"stopTriggerNameEnds", 42}, {"stopCtxEnds", 42}};

        const Tag tag(0, {{gr::tag::TRIGGER_NAME.shortKey(), std::string("alarm")}, {gr::tag::CONTEXT.shortKey(), std::string("room1")}});
        expect(nothrow([&] { expect(eq(trigger::BasicTriggerNameCtxMatcher::filter(filter, tag, state), Ignore)); }));
        expect(nothrow([&] { expect(!trigger::BasicTriggerNameCtxMatcher::isSingleTrigger(state)); }));
    };
};

int main() { /* not needed for UT */ }
