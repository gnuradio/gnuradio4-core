#!/bin/sh
# Asserts that a consumer of Runtime.hpp instantiates none of the framework's per-type machinery.
#
# Compiles qa_RuntimeConsumer.cpp, which includes Runtime.hpp and no other GNU Radio 4 header, with clang -ftime-trace.
# Fails if any InstantiateFunction or InstantiateClass event names an entity from the list below. The gate reads the
# set of names and never a duration. Scheduling noise cannot fail it.
#
# Usage: check_runtime_cascade.sh <source-dir> <core-include-dir> <generated-include-dir> <meta-include-dir> [extra -I dirs...]

set -eu

if [ "$#" -lt 4 ]; then
    echo "usage: $0 <source-dir> <core-include> <generated-include> <meta-include> [extra include dirs...]" >&2
    exit 2
fi

SOURCE_DIR=$1
shift
# the include flags replace the directories in the positional parameters, each one word at the compiler call below
DIR_COUNT=$#
for dir in "$@"; do
    set -- "$@" "-I$dir"
done
shift "$DIR_COUNT"

CXX=${CXX_FOR_CASCADE_GATE:-clang++}
if ! command -v "$CXX" >/dev/null 2>&1; then
    echo "$CXX not found -- the cascade gate needs clang for -ftime-trace" >&2
    exit 2
fi

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

TRACE="$WORK/consumer.json"

# -ftime-trace needs a real compilation. The object is thrown away with the work directory.
"$CXX" -std=c++23 "-ftime-trace=$TRACE" -ftime-trace-granularity=0 -c \
    "$@" "$SOURCE_DIR/qa_RuntimeConsumer.cpp" -o "$WORK/consumer.o" 2>"$WORK/compile.log" || {
    echo "the demo consumer did not compile:" >&2
    cat "$WORK/compile.log" >&2
    exit 1
}

if [ ! -f "$TRACE" ]; then
    echo "clang produced no -ftime-trace output" >&2
    exit 1
fi

FORBIDDEN="gr::Block<
gr::CtxSettings<
gr::scheduler::SchedulerBase<
gr::scheduler::Simple<
gr::scheduler::BreadthFirst<
gr::scheduler::DepthFirst<
gr::GraphWrapper<
gr::BlockWrapper<
gr::SchedulerWrapper<
gr::Port<
gr::lifecycle::StateMachine<"

"${PYTHON_FOR_CASCADE_GATE:-python3}" - "$TRACE" <<PY
import json, sys

forbidden = """$FORBIDDEN""".splitlines()
events = json.load(open(sys.argv[1]))["traceEvents"]

instantiated = {
    ev.get("args", {}).get("detail", "")
    for ev in events
    if ev.get("name") in ("InstantiateFunction", "InstantiateClass")
}
instantiated.discard("")

hits = sorted(name for name in instantiated if any(pattern in name for pattern in forbidden))
print(f"cascade gate: {len(instantiated)} template instantiations in the demo consumer, {len(hits)} forbidden")
for name in hits[:20]:
    print(f"  {name[:160]}")
sys.exit(1 if hits else 0)
PY
