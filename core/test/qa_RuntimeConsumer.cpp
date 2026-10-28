#include <gnuradio-4.0/Runtime.hpp>

#include "qa_RuntimeConsumerBlocks.hpp"

#include <cstdio>
#include <cstdlib>

/**
 * The wiring half of the demo consumer. It builds, runs and watches a three-block graph. Runtime.hpp is its only
 * GNU Radio header.
 *
 * check_runtime_cascade.sh compiles this file with clang -ftime-trace. It asserts that the file instantiates no
 * Block<T>, CtxSettings<T>, Port<T, ...> or SchedulerBase<...>. Nothing here includes another GNU Radio header, and
 * nothing names a framework type.
 */

int main() {
    if (!qa_consumer::registerDemoBlocks()) {
        std::puts("demo blocks could not be registered");
        return EXIT_FAILURE;
    }

    gr::RuntimeGraph graph;

    const auto source = graph.emplace("demo::Counter", "source", {{"n_samples", 1024U}});
    const auto gain   = graph.emplace("demo::Gain", "gain", {{"gain", 2.0f}});
    const auto sink   = graph.emplace("demo::Total", "sink");
    if (!source || !gain || !sink) {
        std::puts("a demo block could not be created");
        return EXIT_FAILURE;
    }

    if (!graph.connect(*source, source->outputPortNames().front(), *gain, "in") || !graph.connect(*gain, "out", *sink, "in")) {
        std::puts("the demo graph could not be wired");
        return EXIT_FAILURE;
    }

    auto runtime = gr::Runtime::create(std::move(graph));
    if (!runtime) {
        std::printf("runtime: %s\n", runtime.error().message.c_str());
        return EXIT_FAILURE;
    }
    if (const auto sent = runtime->send(gr::RuntimeCommand::Get, gain->uniqueName(), gr::Runtime::kSettingsEndpoint, {}, "demo"); !sent) {
        std::printf("send: %s\n", sent.error().message.c_str());
        return EXIT_FAILURE;
    }
    if (const auto ran = runtime->runAndWait(); !ran) {
        std::printf("run: %s\n", ran.error().message.c_str());
        return EXIT_FAILURE;
    }

    bool answered = false;
    for (const gr::RuntimeEvent& event : runtime->pollEvents()) {
        answered = answered || (event.clientRequestID == "demo" && !event.isError);
    }
    if (!answered) {
        std::puts("the demo gain did not answer its settings request");
        return EXIT_FAILURE;
    }

    // 2 * sum(0 .. 1023)
    constexpr double kExpected = 2.0 * 1023.0 * 1024.0 / 2.0;
    if (qa_consumer::totalSamples() != 1024UZ || qa_consumer::total() != kExpected) {
        std::printf("demo graph produced %zu samples summing to %f, expected 1024 and %f\n", qa_consumer::totalSamples(), qa_consumer::total(), kExpected);
        return EXIT_FAILURE;
    }

    std::printf("demo consumer: %zu samples, sum %.0f\n", qa_consumer::totalSamples(), qa_consumer::total());
    return EXIT_SUCCESS;
}
