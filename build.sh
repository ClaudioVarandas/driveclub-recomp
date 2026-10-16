#!/usr/bin/env bash
# Builds the game runtime (out/DriveClubGame.exe) with the MSYS2 CLANG64 toolchain. See BUILDING.md.
set -euo pipefail
cd -- "$(dirname -- "$0")"
mkdir -p out
# shellcheck source=msys2-env.sh
source ./msys2-env.sh
for tool in clang cmake ninja pkg-config; do
    command -v "$tool" >/dev/null || { echo "Need $tool from MSYS2 CLANG64 (see BUILDING.md)." >&2; exit 1; }
done
# Source paths in the binary (assert and log messages) are written relative to the repository.
root="$(cygpath -m "$PWD")"
cmake -S gpu -B out/gpu -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DBB_PGO=off \
    -DBB_LTO="${BB_LTO:-OFF}" -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
    -DCMAKE_C_FLAGS="-ffile-prefix-map=$root=." -DCMAKE_CXX_FLAGS="-ffile-prefix-map=$root=." >/dev/null
if ! ninja -C out/gpu DriveClubGame > out/gpu-build.log 2>&1; then
    grep -v '^\[' out/gpu-build.log | tail -40 >&2
    echo 'Build failed (full log: out/gpu-build.log)' >&2
    exit 1
fi
cp out/gpu/DriveClubGame.exe out/
echo "Built $PWD/out/DriveClubGame.exe"
