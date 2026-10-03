#!/usr/bin/env bash
# Private CI dependency: patched Jammy lavapipe, never installed in a release.
set -euo pipefail

root="$(cd "$(dirname "$0")/.." && pwd)"
prefix="${1:?Usage: build_ci_lavapipe.sh ABSOLUTE_INSTALL_PREFIX}"
[[ "$prefix" = /* ]] || { echo 'Install prefix must be absolute' >&2; exit 1; }
[[ "$(uname -m)" = x86_64 ]] || { echo 'This CI recipe requires x86_64' >&2; exit 1; }
patch_file="$root/scripts/ci/mesa-l3-cleanup.patch"
source_sha=c1458ab511b87d60644b2c46d67f7d9d5c0e3db691db6aae11762b1cc5b43ca0
ubuntu_sha=a870171e601fc9bf99751ce315bacf68a0d57fe4f386a22626111be5dac572c0
recipe="$( { printf '%s\n' "$source_sha" "$ubuntu_sha";
  sha256sum "$root/scripts/build_ci_lavapipe.sh" "$patch_file" | awk '{print $1}';
  gcc --version; g++ --version; llvm-config-15 --version; meson --version;
} | sha256sum | awk '{print $1}')"

verify_prefix() {
  test "$(cat "$prefix/recipe.sha256")" = "$recipe"
  test -s "$prefix/lib/libvulkan_lvp.so"
  test -s "$prefix/share/vulkan/icd.d/lvp_icd.x86_64.json"
  (cd "$prefix" && sha256sum --check files.sha256)
  # Never silently fall back to the leaking system ICD or a relocated cache.
  python - "$prefix" <<'PY'
import json
from pathlib import Path
import sys
p = Path(sys.argv[1])
icd = json.loads((p / 'share/vulkan/icd.d/lvp_icd.x86_64.json').read_text())
assert Path(icd['ICD']['library_path']).resolve() == (p / 'lib/libvulkan_lvp.so').resolve()
PY
  if ldd "$prefix/lib/libvulkan_lvp.so" | grep -q 'not found'; then
    echo 'Cached lavapipe has missing dependencies' >&2
    return 1
  fi
  # The destructor must survive section GC in the final ICD, not just source patching.
  nm -a "$prefix/lib/libvulkan_lvp.so" | grep ' util_cpu_caps_cleanup_on_unload$'
}

if [[ -f "$prefix/recipe.sha256" ]]; then
  verify_prefix
  echo 'PASS: verified cached CI-only lavapipe with L3 unload cleanup'
  exit 0
fi

download="$root/.ci/mesa-downloads"
source="$root/.ci/mesa-source/mesa-23.2.1"
build="$root/.ci/mesa-build"
mkdir -p "$download" "$(dirname "$source")" "$prefix"
[[ ! -e "$source" && ! -e "$build" ]] || {
  echo 'Refusing to patch/reconfigure an existing Mesa source or build tree' >&2; exit 1;
}
base=https://archive.ubuntu.com/ubuntu/pool/main/m/mesa
curl --fail --location --retry 5 "$base/mesa_23.2.1.orig.tar.gz" -o "$download/mesa.tar.gz"
curl --fail --location --retry 5 "$base/mesa_23.2.1-1ubuntu3.1~22.04.4.diff.gz" -o "$download/ubuntu.diff.gz"
printf '%s  %s\n' "$source_sha" "$download/mesa.tar.gz" \
  "$ubuntu_sha" "$download/ubuntu.diff.gz" | sha256sum --check
tar -xzf "$download/mesa.tar.gz" -C "$(dirname "$source")"
gzip -dc "$download/ubuntu.diff.gz" | patch --batch --fuzz=0 -p1 -d "$source"
# Apply the complete reviewed Ubuntu series, including the CVE-2026-40393 fixes.
while IFS= read -r name || [[ -n "$name" ]]; do
  [[ -z "$name" || "$name" = \#* ]] && continue
  [[ "$name" != *' '* && "$name" != */../* ]] || exit 1
  patch --batch --fuzz=0 -p1 -d "$source" < "$source/debian/patches/$name"
done < "$source/debian/patches/series"
patch --batch --fuzz=0 -p1 -d "$source" < "$patch_file"

# Build just software Vulkan, without OpenGL/windowing/video/other hardware ICDs.
# Keep the driver's allocation/free interceptable and its symbols available.
PATH="/usr/lib/llvm-15/bin:$PATH" CC=gcc CXX=g++ LLVM_CONFIG=/usr/bin/llvm-config-15 meson setup "$build" "$source" \
  --prefix="$prefix" --libdir=lib --buildtype=debugoptimized --wrap-mode=nofallback \
  -Dplatforms=[] -Dgallium-drivers=swrast -Dvulkan-drivers=swrast \
  -Dllvm=enabled -Dshared-llvm=enabled -Dopengl=false -Dgles1=disabled \
  -Dgles2=disabled -Dglx=disabled -Degl=disabled -Dgbm=disabled \
  -Dshared-glapi=disabled -Dosmesa=false -Dgallium-xa=disabled \
  -Dgallium-nine=false -Dgallium-opencl=disabled -Dgallium-rusticl=false \
  -Dgallium-va=disabled -Dgallium-vdpau=disabled -Dgallium-omx=disabled \
  -Dvulkan-layers=[] -Dvideo-codecs=[] -Dtools=[] -Dbuild-tests=false \
  -Dc_args=-fno-omit-frame-pointer -Dcpp_args=-fno-omit-frame-pointer
meson compile -C "$build" -j 2
meson install -C "$build"
cp "$source/docs/license.rst" "$prefix/MESA-LICENSE.rst"
printf '%s\n' "$recipe" > "$prefix/recipe.sha256"
(cd "$prefix" && sha256sum lib/libvulkan_lvp.so \
  share/vulkan/icd.d/lvp_icd.x86_64.json MESA-LICENSE.rst > files.sha256)
verify_prefix
echo 'PASS: built CI-only lavapipe with L3 unload cleanup; live LSan gates follow'
