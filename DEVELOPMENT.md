# GNU Radio 4 Core Development

`gnuradio4-core` contains the GNU Radio 4 core runtime and SDK. It is not the
former top-level GNU Radio 4 development tree. Downstream repositories such as
`gnuradio4-library` and `gnuradio4-blocks` build against an installed core SDK.

## Source

```bash
git clone git@github.com:gnuradio/gnuradio4-core.git
cd gnuradio4-core
```

## Recommended: CI Container

The most reproducible local development environment is one of the same
`gnuradio/ci` images used by GitHub Actions.

```bash
docker run --rm -it \
  --user "$(id -u):$(id -g)" \
  --volume "$PWD:/work/src" \
  --workdir /work/src \
  ghcr.io/gnuradio/ci:ubuntu-24.04-4.0 \
  bash
```

Inside the container:

```bash
cmake -S . -B build \
  -DCMAKE_BUILD_TYPE=Debug \
  -DGR_USE_FETCHCONTENT_DEPS=ON
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

To test another supported profile, use the corresponding CI image and compiler
tuple, for example:

```bash
CC=clang-20 CXX=clang++-20 cmake -S . -B build-clang \
  -DCMAKE_BUILD_TYPE=Release \
  -DGR_USE_FETCHCONTENT_DEPS=ON \
  -DADDRESS_SANITIZER=ON
cmake --build build-clang --parallel
ctest --test-dir build-clang --output-on-failure
```

## Native Linux Development

Required tools:

- CMake >= 3.27
- GCC >= 14 or Clang >= 20
- Git
- pkg-config
- Ninja or GNU Make

Native package availability varies by distribution. If Boost.UT, cpp-httplib, or
vir-simd are not packaged for your system, configure with
`-DGR_USE_FETCHCONTENT_DEPS=ON`.

```bash
cmake -S . -B build \
  -DCMAKE_BUILD_TYPE=Debug \
  -DGR_USE_FETCHCONTENT_DEPS=ON
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

Install the core SDK for downstream development:

```bash
cmake --install build --prefix "$HOME/gr4-core"
```

Downstream repositories should then configure with:

```bash
cmake -S . -B build -DCMAKE_PREFIX_PATH="$HOME/gr4-core"
```

## Emscripten

Emscripten builds are covered by CI using
`ghcr.io/gnuradio/ci:ubuntu-24.04-4.0-emscripten`. Prefer that image unless you
already maintain a local Emscripten SDK installation.

```bash
docker run --rm -it \
  --user "$(id -u):$(id -g)" \
  --volume "$PWD:/work/src" \
  --workdir /work/src \
  ghcr.io/gnuradio/ci:ubuntu-24.04-4.0-emscripten \
  bash
```

Inside the container:

```bash
export SYSTEM_NODE="$(which node)"
"$EMSDK_HOME/emsdk" activate "$EMSDK_VERSION"
source "$EMSDK_HOME/emsdk_env.sh"

emcmake cmake -S . -B build-wasm \
  -DCMAKE_BUILD_TYPE=Release \
  -DENABLE_COVERAGE=OFF \
  -DCMAKE_CROSSCOMPILING_EMULATOR="$SYSTEM_NODE" \
  -DGNURADIO_PARSE_REGISTRATIONS_TOOL_CXX_COMPLILER=g++-14
cmake --build build-wasm --parallel
ctest --test-dir build-wasm --output-on-failure
```

## macOS

macOS CI uses Homebrew `llvm@20`. Local development should follow the same
toolchain where possible.

```bash
brew install llvm@20 cmake ninja

CC=/opt/homebrew/opt/llvm@20/bin/clang \
CXX=/opt/homebrew/opt/llvm@20/bin/clang++ \
cmake -S . -B build-macos \
  -DCMAKE_BUILD_TYPE=Debug \
  -DGR_USE_FETCHCONTENT_DEPS=ON
cmake --build build-macos --parallel
ctest --test-dir build-macos --output-on-failure
```

Adjust the Homebrew prefix if your system uses `/usr/local` instead of
`/opt/homebrew`.

## Windows

Windows support is best treated as experimental unless CI coverage is added for
the exact environment you are using. The historical MSYS2/UCRT64 and CLANG64
setup may still be useful, but it is not the primary validated development path.

For local experiments, install Git, CMake, Ninja, a C++23-capable compiler, and
the same third-party dependencies listed above, or use FetchContent where
possible:

```bash
cmake -S . -B build ^
  -DCMAKE_BUILD_TYPE=Debug ^
  -DGR_USE_FETCHCONTENT_DEPS=ON ^
  -DWARNINGS_AS_ERRORS=OFF
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

## Formatting

The repository includes formatting helpers:

```bash
./formatFiles.sh
./formatLastCommit.sh
```

They expect compatible `clang-format` and `cmake-format` versions to be
available on `PATH`. Restyled also reports formatting patches in CI when files
need mechanical cleanup.

## Useful CMake Options

- `-DGR_USE_FETCHCONTENT_DEPS=ON`: fetch missing third-party dependencies.
- `-DUSE_CCACHE=OFF`: disable local compiler cache integration.
- `-DADDRESS_SANITIZER=ON`: enable AddressSanitizer.
- `-DUB_SANITIZER=ON`: enable UndefinedBehaviorSanitizer.
- `-DTHREAD_SANITIZER=ON`: enable ThreadSanitizer.
- `-DWARNINGS_AS_ERRORS=OFF`: useful for unsupported local toolchains.
- `-DCMAKE_EXPORT_COMPILE_COMMANDS=ON`: generate `compile_commands.json`.
