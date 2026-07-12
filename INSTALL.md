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
- Boost.UT development package or installed headers
- cpp-httplib development package
- vir-simd headers
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

Downstream repositories should configure with:

```bash
cmake -S . -B build -DCMAKE_PREFIX_PATH="$HOME/gr4-core"
```

For CI or containerized downstream development, use the published core SDK image
instead of a local install. See [docs/ci/sdk-image.md](docs/ci/sdk-image.md).

## Ubuntu 24.04 (Dependencies)

```bash
sudo apt update
sudo apt install -y cmake g++ git libcpp-httplib-dev pkg-config python3 python3-dev
```

Ubuntu 24.04 does not package all native dependencies used by the default core build. Install Boost.UT and vir-simd
from source or use the GNU Radio CI builder image. For an explicit online fallback, configure with
`-DGR_USE_FETCHCONTENT_DEPS=ON`.

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
