#!/usr/bin/env bash
#
# Builds a release .deb of workbench for hand-off to beta testers.
#
#   ./package.sh              -> dist/workbench_0.1.0~beta_amd64.deb
#   ./package.sh 0.2.0        -> version 0.2.0, still a ~beta package
#   ./package.sh 1.0.0 ""     -> a final (non-beta) 1.0.0 package
#
# The build tree lives in build-release/ so it never disturbs the
# incremental Debug tree in build/ that you develop against.

set -euo pipefail

cd "$(dirname "$0")"

VERSION="${1:-0.1.0}"
SUFFIX="${2-beta}"
BUILD_DIR="build-release"
DIST_DIR="dist"

require() {
    command -v "$1" >/dev/null 2>&1 || {
        echo "error: '$1' is required but not installed." >&2
        echo "       sudo apt install $2" >&2
        exit 1
    }
}

require cmake cmake
require dpkg-shlibdeps dpkg-dev
require fakeroot fakeroot
require python3 python3

echo "==> Rendering icons"
python3 packaging/render_icons.py

echo "==> Configuring ${BUILD_DIR} (Release, tests off)"
cmake -B "$BUILD_DIR" -S . \
    -DCMAKE_BUILD_TYPE=Release \
    -DWORKBENCH_BUILD_TESTS=OFF \
    -DWORKBENCH_VERSION="$VERSION" \
    -DWORKBENCH_VERSION_SUFFIX="$SUFFIX" \
    >/dev/null

echo "==> Building"
cmake --build "$BUILD_DIR" -j"$(nproc)"

echo "==> Packaging"
( cd "$BUILD_DIR" && cpack )

mkdir -p "$DIST_DIR"
find "$BUILD_DIR" -maxdepth 1 -name '*.deb' -exec mv -f {} "$DIST_DIR/" \;

DEB="$(find "$DIST_DIR" -maxdepth 1 -name '*.deb' -newermt '-2 minutes' | head -n1)"

echo
echo "Built: ${DEB}"
echo
echo "Depends: $(dpkg-deb -f "$DEB" Depends)"
echo
echo "Send testers that one file. They install it with:"
echo "    sudo apt install ./$(basename "$DEB")"
echo "then launch 'Workbench' from the applications menu, or run 'workbench'."
