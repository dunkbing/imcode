#!/bin/sh
# a release build as dist/ImCode-<arch>.AppImage (x86_64 or aarch64) and its .sha256.
# VCPKG_ROOT must point at vcpkg; zig >= 0.17 on PATH (libghostty-vt builds from source here).
# auto-update: IMCODE_MANIFEST_URL is the json manifest the AppImage checks (see imshell's
# docs/auto-update.md); without it the app does not update itself. IMCODE_VERSION overrides the
# project's version (a test build of an update)
set -e
cd "$(dirname "$0")/.."
: "${VCPKG_ROOT:?set VCPKG_ROOT}"
version=${IMCODE_VERSION:-$(sed -n 's/^project(imcode VERSION \([0-9.]*\).*/\1/p' CMakeLists.txt)}
arch=$(uname -m)
cmake -S . -B build-release -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_TOOLCHAIN_FILE="$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake" \
    -DIMCODE_UPDATER=ON -DIMCODE_VERSION="$version" -DIMCODE_MANIFEST_URL="$IMCODE_MANIFEST_URL"
cmake --build build-release --target imcode

# linuxdeploy bundles the libraries; the type2 runtime runs with fuse2, fuse3 or none
tools=build-release/appimage-tools
mkdir -p "$tools"
[ -x "$tools/linuxdeploy" ] || { curl -fsSL -o "$tools/linuxdeploy" \
    "https://github.com/linuxdeploy/linuxdeploy/releases/download/continuous/linuxdeploy-$arch.AppImage"; chmod +x "$tools/linuxdeploy"; }
[ -f "$tools/runtime" ] || curl -fsSL -o "$tools/runtime" \
    "https://github.com/AppImage/type2-runtime/releases/download/continuous/runtime-$arch"

appdir=build-release/AppDir
rm -rf "$appdir" dist && mkdir -p "$appdir/usr/bin" dist
install -m755 build-release/imcode "$appdir/usr/bin/imcode"
cat > build-release/imcode.desktop <<DESKTOP
[Desktop Entry]
Type=Application
Name=ImCode
Exec=imcode %F
Icon=imcode
Categories=Development;
DESKTOP
out="dist/ImCode-$arch.AppImage"
APPIMAGE_EXTRACT_AND_RUN=1 OUTPUT="$out" LDAI_RUNTIME_FILE="$tools/runtime" "$tools/linuxdeploy" \
    --appdir "$appdir" --desktop-file build-release/imcode.desktop --icon-file assets/icon/imcode-256.png --output appimage
sha256sum "$out" > "$out.sha256"
echo "$out ($version)"
