#!/usr/bin/env bash
# Builds VeeaStats-x86_64.AppImage in the project's dist/ folder.
#
#   packaging/build-appimage.sh              build with this machine's compiler
#   packaging/build-appimage.sh --container  build inside Ubuntu 22.04 (podman or docker)
#
# Use --container for releases: a binary only runs on distros whose glibc is at
# least as new as the one it was built against, so building on an older distro
# makes the AppImage run on far more systems.
set -euo pipefail

PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_IMAGE="ubuntu:22.04"
# A fixed linuxdeploy release, checked against its SHA256 before it is run (as
# CMakeLists.txt does for GLFW). To update: pick a newer tagged release and
# take the new checksum from its release page.
LINUXDEPLOY_URL="https://github.com/linuxdeploy/linuxdeploy/releases/download/1-alpha-20251107-1/linuxdeploy-x86_64.AppImage"
LINUXDEPLOY_SHA256="c20cd71e3a4e3b80c3483cef793cda3f4e990aca14014d23c544ca3ce1270b4d"

if [[ "${1:-}" == "--container" ]]; then
    if command -v podman >/dev/null; then engine=podman
    elif command -v docker >/dev/null; then engine=docker
    else echo "error: --container needs podman or docker installed" >&2; exit 1
    fi
    exec "$engine" run --rm -v "$PROJECT_DIR:/src:Z" -w /src "$BUILD_IMAGE" bash -c '
        set -e
        export DEBIAN_FRONTEND=noninteractive
        apt-get update -qq
        apt-get install -y -qq --no-install-recommends \
            build-essential cmake pkg-config curl ca-certificates file \
            libgl-dev libx11-dev libxrandr-dev libxinerama-dev libxcursor-dev libxi-dev \
            libxkbcommon-dev libwayland-dev libwayland-bin libglib2.0-dev >/dev/null
        BUILD_DIR_NAME=build-appimage-container packaging/build-appimage.sh
        # Docker runs as real root, so hand the output back to the user. (Rootless
        # podman already maps container root to the user; chown there would break it.)
        if [ "'"$engine"'" = docker ]; then chown -R '"$(id -u):$(id -g)"' dist build-appimage-container; fi
    '
fi

# The container gets its own build folder: CMake caches can't be shared between paths.
BUILD_DIR="$PROJECT_DIR/${BUILD_DIR_NAME:-build-appimage}"
APPDIR="$BUILD_DIR/AppDir"
TOOLS_DIR="$BUILD_DIR/tools"
DIST_DIR="$PROJECT_DIR/dist"

cmake -S "$PROJECT_DIR" -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr
cmake --build "$BUILD_DIR" -j"$(nproc)"

# The release binary must carry its own C++ runtime (CMakeLists.txt links it
# in when it can), or it would depend on each user's libstdc++ version.
if objdump -p "$BUILD_DIR/CpuMonitor" | grep -q 'NEEDED.*libstdc++'; then
    echo "error: CpuMonitor links libstdc++ dynamically; install the static libstdc++ (libstdc++-static / libstdc++-dev)" >&2
    exit 1
fi
rm -rf "$APPDIR"
DESTDIR="$APPDIR" cmake --install "$BUILD_DIR"

mkdir -p "$TOOLS_DIR" "$DIST_DIR"
LINUXDEPLOY="$TOOLS_DIR/linuxdeploy-x86_64.AppImage"
if ! echo "$LINUXDEPLOY_SHA256  $LINUXDEPLOY" | sha256sum --check --status 2>/dev/null; then
    curl -fL --progress-bar -o "$LINUXDEPLOY.download" "$LINUXDEPLOY_URL"
    if ! echo "$LINUXDEPLOY_SHA256  $LINUXDEPLOY.download" | sha256sum --check --status; then
        echo "error: linuxdeploy download doesn't match its expected SHA256; not running it" >&2
        rm -f "$LINUXDEPLOY.download"
        exit 1
    fi
    mv "$LINUXDEPLOY.download" "$LINUXDEPLOY"
    chmod +x "$LINUXDEPLOY"
fi

# Containers usually have no FUSE, so let linuxdeploy unpack itself instead.
export APPIMAGE_EXTRACT_AND_RUN=1
export OUTPUT="$DIST_DIR/VeeaStats-x86_64.AppImage"
# AppRun (packaging/AppRun) installs the app for the user on first open.
# linuxdeploy copies any non-system libraries the binary needs into the AppImage;
# OpenGL, X11 and glibc are deliberately left out and come from the user's system.
"$LINUXDEPLOY" --appdir "$APPDIR" \
    --desktop-file "$APPDIR/usr/share/applications/veeastats.desktop" \
    --icon-file "$APPDIR/usr/share/icons/hicolor/scalable/apps/veeastats.svg" \
    --custom-apprun "$PROJECT_DIR/packaging/AppRun" \
    --output appimage

echo
echo "Built: $OUTPUT"
