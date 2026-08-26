#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 2 ]]; then
  echo "usage: $0 <core-build-dir> <install-prefix>" >&2
  exit 2
fi

build_dir=$1
install_prefix=$2
script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
smoke_build_dir="${build_dir}/installed-sdk-smoke"

cmake --install "${build_dir}" --prefix "${install_prefix}"
cmake -S "${script_dir}/minimal_blocklib" -B "${smoke_build_dir}" -DCMAKE_PREFIX_PATH="${install_prefix}"
cmake --build "${smoke_build_dir}"

# building verifies the headers and link line; running verifies the generated registration path
for linkage in Shared Static; do
  "${smoke_build_dir}/minimal_blocklib_consumer_${linkage}"
done
