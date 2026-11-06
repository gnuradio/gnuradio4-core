#ifndef GNURADIO_TOOLS_RUNGRAPH_HPP
#define GNURADIO_TOOLS_RUNGRAPH_HPP

namespace gr::tools {

/**
 * @brief Runs the rungraph command line: loads the graph file it names, runs the graph and reports what it was asked to.
 *
 * `argc` and `argv` are those `main` receives. `argv[0]` is skipped, and each message begins with `rungraph:`. The
 * options are those `rungraph --help` lists. The return value is the exit status: 0 for a run that met what it was asked to, 1 for a graph that could not be
 * read, loaded or run or a check that failed, 2 for a command line, scheduler or scheduler setting that could not be used.
 *
 * It registers core's `Simple` scheduler under its four keys before it searches the plugin directories. The graph's
 * blocks come from the block registry and from the plugins of those directories. A program built without the plugin
 * system links its block libraries and calls each library's generated initializer with `gr::globalBlockRegistry()`
 * before this call, such as `gr::blocklib::initMyBlocks(gr::globalBlockRegistry())`. The call registers the blocks and
 * keeps a static library's objects in the link.
 *
 * While the graph runs, SIGINT and SIGTERM stop it as `--seconds` does. The call puts back the handlers it replaced
 * before it returns, and a signal to an earlier call does not stop a later one. The handlers it saves and puts back are
 * those `std::signal` installs: a host handler set through `sigaction` with flags gets its function back without those
 * flags. Calls do not overlap in time.
 */
[[nodiscard]] int runGraph(int argc, const char* const* argv);

} // namespace gr::tools

#endif // GNURADIO_TOOLS_RUNGRAPH_HPP
