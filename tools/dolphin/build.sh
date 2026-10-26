#!/bin/sh
set -eu

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
cache_root=${SMG_DOLPHIN_CACHE:-/home/utking/.cache/smg-dolphin}
build_root=$cache_root/build
source_dir=$build_root/source
cmake_dir=$build_root/cmake
revision=$(tr -d '[:space:]' < "$script_dir/../dolphin-revision.txt")
image=${SMG_DOLPHIN_BUILDER_IMAGE:-smg-dolphin-bookworm-builder}

case "$cache_root" in
  /home/utking/.cache/smg-dolphin|/home/utking/.cache/smg-dolphin/*) ;;
  *) echo "build.sh: SMG_DOLPHIN_CACHE must stay under /home/utking/.cache/smg-dolphin" >&2; exit 2 ;;
esac

rm -rf "$build_root"
mkdir -p "$build_root"
docker build --tag "$image" --file "$script_dir/Dockerfile" "$script_dir"
git clone --filter=blob:none https://github.com/dolphin-emu/dolphin.git "$source_dir"
git -C "$source_dir" checkout --detach "$revision"
git -C "$source_dir" submodule update --init --recursive
git -C "$source_dir" apply "$script_dir/capture.patch"
mkdir -p "$cmake_dir"
docker run --rm --user "$(id -u):$(id -g)" \
  -v "$source_dir:/src:ro" -v "$cmake_dir:/build" "$image" \
  cmake -S /src -B /build -GNinja -DCMAKE_BUILD_TYPE=Release \
    -DENABLE_QT=OFF -DENABLE_WX=OFF -DENABLE_TESTS=OFF
docker run --rm --user "$(id -u):$(id -g)" \
  -v "$source_dir:/src:ro" -v "$cmake_dir:/build" "$image" \
  cmake --build /build --target dolphin-emu-nogui --parallel "${SMG_DOLPHIN_JOBS:-$(nproc)}"
install -D -m 0755 "$cmake_dir/Binaries/dolphin-emu-nogui" "$build_root/dolphin-emu-nogui"
sha256sum "$build_root/dolphin-emu-nogui"
