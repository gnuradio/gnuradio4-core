# GNU Radio 4 Core - Installation Guide

At the moment GNU Radio 4 core is not yet packaged and has to be installed from
source. This document describes the steps and prerequisites to build, test, and
install `gnuradio4-core` for runtime development and downstream block-library
development.

This repository contains the core runtime and SDK only. Standard blocks and
reusable libraries live in downstream repositories such as `gnuradio4-library`
and `gnuradio4-blocks`.

As the installation and packaging matures, these instructions are expected to be extended.

## Requirements

- CMake ≥ 3.27
- C++23 compatible compiler
  - GCC ≥ 14.2 (Linux)
- Git
- pkg-config
- magic_enum >= 0.9.3
- vir-simd >= 0.4.4
- Boost.UT >= 2.3.1, only when building tests and benchmarks
- cpp-httplib >= 0.18.1, only when building tests natively
- Python 3 (optional)

Verify tool versions:

```bash
cmake --version
g++ --version
```

## Clone Repository

```bash
git clone https://github.com/gnuradio/gnuradio4-core.git
cd gnuradio4-core
```

## Build

GNU Radio 4 core uses an out-of-source CMake build.

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
```

## Run Tests

```bash
ctest --test-dir build --output-on-failure
```

## Install Core SDK

```bash
cmake --install build --prefix "$HOME/gr4-core"
```

The plugin loader keeps its cached data in a data cache directory that no
build compiles in. The `GR_DATA_CACHE_DIR` environment variable names it when
it is set and not empty. Otherwise the loader uses the user's cache directory,
`gnuradio4/cache` under `$XDG_CACHE_HOME` or else under `$HOME/.cache`. When
neither can be used, caching is off and the loader prints a warning that names
the reason. `grinfo version` reports the directory in use.

Downstream repositories should configure with:

```bash
cmake -S . -B build -DCMAKE_PREFIX_PATH="$HOME/gr4-core"
```

The installed package finds magic_enum and vir-simd for every consumer. A consumer that configures with
`ENABLE_TESTING` set also needs Boost.UT (>= 2.3.1), because the installed `UnitTestHelper.hpp` and
`gnuradio4::ut-benchmark` include it.

For CI or containerized downstream development, use the published core SDK image
instead of a local install. See [docs/ci/sdk-image.md](docs/ci/sdk-image.md).

## Ubuntu 24.04 (Dependencies)

```bash
sudo apt update
sudo apt install -y cmake g++ git pkg-config python3 python3-dev
```

Ubuntu 24.04 does not package all native dependencies used by the default core build. Install magic_enum
(>= 0.9.3), vir-simd (>= 0.4.4) and, for tests, Boost.UT (>= 2.3.1) and cpp-httplib (>= 0.18.1) from source, and pass their install prefix in `CMAKE_PREFIX_PATH`. The
[gr-deps superbuild](https://github.com/johnwparent/gnuradio4-superbuild) builds all four into one prefix.

## macOS via Homebrew

macOS builds are supported by using `llvm@20` from the
[Homebrew package manager](https://brew.sh/). See
[DEVELOPMENT.md](DEVELOPMENT.md#macos) for the tested local workflow.

## Platform Notes

- Linux: Expected to work with a modern toolchain
- Windows: experimental; see [DEVELOPMENT.md](DEVELOPMENT.md#windows)
- macOS: supported using llvm@20 from homebrew

## Troubleshooting

- Ensure CMake ≥ 3.27
- Ensure compiler supports C++23
- Check logs:
  - CMakeFiles/CMakeError.log
  - CMakeFiles/CMakeOutput.log

For a reproducible setup, see Docker workflow:
[DEVELOPMENT.md](DEVELOPMENT.md#recommended-ci-container)
