# GNU Radio 4 SDK Image

The GNU Radio 4 SDK image is published to GHCR on pushes to `main` by
`.github/workflows/sdk-image.yml`. It is intended for downstream out-of-tree
module development and CI, not as a user-facing binary installer.

The image is based on the GNU Radio Ubuntu 26.04 CI image:

```text
ghcr.io/gnuradio/ci:ubuntu-26.04-4.0
```

It uses that image's default GCC/G++ toolchain and installs GNU Radio 4 under:

```text
/opt/gnuradio4
```

Downstream builds should set `CMAKE_PREFIX_PATH=/opt/gnuradio4`. For
reproducible CI, pin the image to a full git SHA plus profile tag. The
`main-<profile>` tags are moving convenience tags and should not be treated as
reproducible.

Published tags:

```text
ghcr.io/<owner>/gnuradio4-core-sdk:<full-git-sha>-ubuntu-24.04-gcc14-release
ghcr.io/<owner>/gnuradio4-core-sdk:main-ubuntu-24.04-gcc14-release
ghcr.io/<owner>/gnuradio4-core-sdk:<full-git-sha>-ubuntu-26.04-gcc-release
ghcr.io/<owner>/gnuradio4-core-sdk:main-ubuntu-26.04-gcc-release
ghcr.io/<owner>/gnuradio4-core-sdk:<full-git-sha>-ubuntu-26.04-gcc-debug
ghcr.io/<owner>/gnuradio4-core-sdk:main-ubuntu-26.04-gcc-debug
ghcr.io/<owner>/gnuradio4-core-sdk:<full-git-sha>-ubuntu-24.04-clang20-release
ghcr.io/<owner>/gnuradio4-core-sdk:main-ubuntu-24.04-clang20-release
ghcr.io/<owner>/gnuradio4-core-sdk:<full-git-sha>-fedora-44-clang-release
ghcr.io/<owner>/gnuradio4-core-sdk:main-fedora-44-clang-release
ghcr.io/<owner>/gnuradio4-core-sdk:<full-git-sha>-fedora-44-clang-debug
ghcr.io/<owner>/gnuradio4-core-sdk:main-fedora-44-clang-debug
```

Example downstream GitHub Actions usage:

```yaml
jobs:
  build:
    runs-on: ubuntu-latest
    container:
      image: ghcr.io/<owner>/gnuradio4-core-sdk:<sha>-ubuntu-26.04-gcc-release

    steps:
      - uses: actions/checkout@v4

      - name: Configure
        run: cmake -S . -B build -DCMAKE_PREFIX_PATH=/opt/gnuradio4

      - name: Build
        run: cmake --build build --parallel
```

Example local container build:

```sh
docker run --rm -it \
  -v "$PWD:/work" \
  -w /work \
  ghcr.io/<owner>/gnuradio4-core-sdk:<sha>-ubuntu-26.04-gcc-release \
  bash -lc 'cmake -S . -B build -DCMAKE_PREFIX_PATH=/opt/gnuradio4 && cmake --build build --parallel'
```
