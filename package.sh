#!/usr/bin/env bash
#
# Builds a release .deb of workbench for hand-off to beta testers.
#
#   ./package.sh              -> dist/workbench_0.1.0~beta_amd64.deb
#   ./package.sh 0.2.0        -> version 0.2.0, still a ~beta package
#   ./package.sh 1.0.0 ""     -> a final (non-beta) 1.0.0 package
#
# Add --publish to also copy the .deb into release/, which is tracked in
# git and is how testers get it:
#
#   ./package.sh --publish
#   ./package.sh --publish 0.2.0
#
# The build tree lives in build-release/ so it never disturbs the
# incremental Debug tree in build/ that you develop against. dist/ is
# ignored scratch output; release/ is the single published artifact.

set -euo pipefail

cd "$(dirname "$0")"

# --publish may appear anywhere; everything else stays positional.
PUBLISH=0
ARGS=()
for arg in "$@"; do
    if [ "$arg" = "--publish" ]; then
        PUBLISH=1
    else
        ARGS+=("$arg")
    fi
done
set -- ${ARGS[@]+"${ARGS[@]}"}

VERSION="${1:-0.1.0}"
SUFFIX="${2-beta}"
BUILD_DIR="build-release"
DIST_DIR="dist"
RELEASE_DIR="release"

# Every build a tester might install has to carry a version string no
# tester has seen before. `apt install ./pkg.deb` is a no-op when the
# installed version already matches, so shipping changed contents under a
# reused version silently leaves the old binary in place and looks like
# the package "didn't take". A UTC build stamp makes each pre-release
# build strictly newer than the last, and the commit makes a tester's
# `dpkg -s workbench` traceable back to exactly what they are running.
#
# A final release (empty suffix) is deliberately left unstamped: 1.0.0
# should be 1.0.0, and it already outranks every 1.0.0~beta+... build.
if [ -n "$SUFFIX" ]; then
    BUILD_STAMP="$(date -u +%Y%m%d%H%M)"
    COMMIT="$(git rev-parse --short HEAD 2>/dev/null || echo unknown)"
    FULL_SUFFIX="${SUFFIX}+${BUILD_STAMP}.g${COMMIT}"
else
    FULL_SUFFIX=""
fi

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
    -DWORKBENCH_VERSION_SUFFIX="$FULL_SUFFIX" \
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

if [ "$PUBLISH" -eq 1 ]; then
    if [ -n "$(git status --porcelain -- . ":(exclude)$RELEASE_DIR" 2>/dev/null)" ]; then
        echo
        echo "warning: publishing from a dirty working tree - the g${COMMIT:-?}" >&2
        echo "         in this package's version does not fully describe it." >&2
    fi

    # Exactly one .deb is ever tracked: drop any previous version first, so
    # the working tree never accumulates stale packages and there is no
    # ambiguity about which file testers should take.
    mkdir -p "$RELEASE_DIR"
    find "$RELEASE_DIR" -maxdepth 1 -name '*.deb' -delete
    cp "$DEB" "$RELEASE_DIR/"

    PUBLISHED="$RELEASE_DIR/$(basename "$DEB")"
    echo
    echo "==> Published to ${PUBLISHED}"
    echo
    echo "Commit and push it to make the release live:"
    echo "    git add -A ${RELEASE_DIR}"
    echo "    git commit -m \"Release $(dpkg-deb -f "$DEB" Version)\""
    echo "    git push"
    echo
    echo "Testers then run:"
    echo "    git pull"
    echo "    sudo apt install ./${PUBLISHED}"
else
    echo
    echo "Send testers that one file. They install it with:"
    echo "    sudo apt install ./$(basename "$DEB")"
    echo "then launch 'Workbench' from the applications menu, or run 'workbench'."
    echo
    echo "Or re-run with --publish to stage it in release/ for distribution via git."
fi
