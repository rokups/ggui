#!/usr/bin/env bash
set -euo pipefail

root=$(cd "$(dirname "$0")/.." && pwd)
build=${1:-"$root/build/coverage"}
gg_prefix=${GGUI_GG_PREFIX:-"$root/../gg/build/install"}

cmake -S "$root" -B "$build" -G Ninja \
  -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_PREFIX_PATH="$gg_prefix" \
  -DGGUI_BUILD_TESTS=ON \
  -DGGUI_ENABLE_IMGUI_TEST_ENGINE=ON \
  -DGGUI_ENABLE_COVERAGE=ON
cmake --build "$build" --parallel

find "$build" -type f -name '*.gcda' -delete
test_root=$(mktemp -d /tmp/ggui-software-tests.XXXXXX)
cleanup() { find "$test_root" -depth -delete; }
trap cleanup EXIT

software() {
  env LIBGL_ALWAYS_SOFTWARE=1 GALLIUM_DRIVER=llvmpipe SDL_AUDIODRIVER=dummy XDG_RUNTIME_DIR=/tmp-run \
    xvfb-run -a "$@"
}

if command -v glxinfo >/dev/null 2>&1; then
  software glxinfo -B | sed -n '/OpenGL renderer string/p'
fi

"$build/bin/ggui_tests"

xdg_main="$test_root/xdg-main"
mkdir -p "$xdg_main"
software env XDG_DATA_HOME="$xdg_main" "$build/bin/ggui" --test
software env XDG_DATA_HOME="$xdg_main" "$build/bin/ggui" --test=PureHelpers ignored-repository

capture_root="$test_root/capture"
xdg_capture="$capture_root/xdg"
mkdir -p "$xdg_capture"
(
  cd "$capture_root"
  software env GGUI_CAPTURE_MANUAL=1 XDG_DATA_HOME="$xdg_capture" "$build/bin/ggui" --test=Presentation
)
test "$(find "$capture_root/output/captures" -type f -name '*.png' | wc -l)" -eq 2

smoke_repo="$test_root/smoke-repository"
git init -q "$smoke_repo"
git -C "$smoke_repo" config user.name "ggui coverage"
git -C "$smoke_repo" config user.email "ggui-coverage@example.test"
printf 'base\n' >"$smoke_repo/tracked.txt"
git -C "$smoke_repo" add tracked.txt
git -C "$smoke_repo" commit -q -m base
software env XDG_DATA_HOME="$xdg_main" "$build/bin/ggui" --smoke "$smoke_repo"

xdg_recent="$test_root/xdg-recent"
mkdir -p "$xdg_recent/gg/ggui"
printf '{"recentRepositories":["%s"],"darkTheme":true,"defaultLayout":true}\n' "$smoke_repo" \
  >"$xdg_recent/gg/ggui/settings.json"
software env XDG_DATA_HOME="$xdg_recent" "$build/bin/ggui" --smoke

xdg_broken="$test_root/xdg-broken"
mkdir -p "$xdg_broken/gg/ggui/settings.json"
printf 'occupied\n' >"$xdg_broken/gg/ggui/settings.json/occupied.txt"
software env XDG_DATA_HOME="$xdg_broken" "$build/bin/ggui" --smoke

env XDG_DATA_HOME="$test_root/xdg-invalid-video" SDL_VIDEODRIVER=does-not-exist SDL_AUDIODRIVER=dummy \
  "$build/bin/ggui" --smoke || true
env XDG_DATA_HOME="$test_root/xdg-dummy-video" SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy \
  "$build/bin/ggui" --smoke || true
software env XDG_DATA_HOME="$test_root/xdg-old-gl" MESA_GL_VERSION_OVERRIDE=2.0 \
  "$build/bin/ggui" --smoke || true

"$root/scripts/coverage_report.py" "$build"
