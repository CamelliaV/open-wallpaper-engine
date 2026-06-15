#!/usr/bin/env bash
set -euo pipefail

PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
ENV_FILE="$PROJECT_DIR/environment.yml"
ENV_NAME="${OWE_CONDA_ENV:-owe-plugin}"
ENV_PREFIX="${OWE_CONDA_PREFIX:-$PROJECT_DIR/build/conda-envs/$ENV_NAME}"
BUILD_TYPE="${BUILD_TYPE:-Release}"
WAYWALLEN_REF="${WAYWALLEN_REF:-main}"
WAYWALLEN_SRC="${WAYWALLEN_SRC:-$PROJECT_DIR/build/waywallen-src}"
BRIDGE_BUILD_DIR="${BRIDGE_BUILD_DIR:-$PROJECT_DIR/build/waywallen-bridge}"
BRIDGE_INSTALL_DIR="${BRIDGE_INSTALL_DIR:-$PROJECT_DIR/build/waywallen-bridge-install}"
OWE_BUILD_DIR="${OWE_BUILD_DIR:-$PROJECT_DIR/build/waywallen-plugin}"
INSTALL_DIR="${INSTALL_DIR:-$PROJECT_DIR/build/waywallen-plugin-install}"
DIST_DIR="${DIST_DIR:-$PROJECT_DIR/dist}"

info() { printf '\n\033[1;36m==> %s\033[0m\n' "$*"; }
fail() { printf '\033[1;31mERROR:\033[0m %s\n' "$*" >&2; exit 1; }

command -v conda >/dev/null || fail "conda not found"
command -v git >/dev/null || fail "git not found"
[[ -f "$ENV_FILE" ]] || fail "missing $ENV_FILE"

export XDG_CACHE_HOME="${XDG_CACHE_HOME:-$PROJECT_DIR/build/.cache}"
export CONDARC="${CONDARC:-$PROJECT_DIR/build/condarc}"
if [[ ! -f "$CONDARC" ]]; then
    mkdir -p "$(dirname "$CONDARC")"
    {
        printf 'channels:\n'
        printf '  - conda-forge\n'
        printf '  - nodefaults\n'
        printf 'channel_priority: strict\n'
        printf 'default_channels: []\n'
        printf 'auto_activate_base: false\n'
    } > "$CONDARC"
fi

case "$INSTALL_DIR" in
    "$PROJECT_DIR"/build/*) ;;
    *) fail "INSTALL_DIR must stay under $PROJECT_DIR/build" ;;
esac
case "$BRIDGE_INSTALL_DIR" in
    "$PROJECT_DIR"/build/*) ;;
    *) fail "BRIDGE_INSTALL_DIR must stay under $PROJECT_DIR/build" ;;
esac

info "Preparing conda env: $ENV_PREFIX"
set +u
# shellcheck disable=SC1091
source "$(conda info --base)/etc/profile.d/conda.sh"
set -u

if [[ -d "$ENV_PREFIX/conda-meta" ]]; then
    conda env update -p "$ENV_PREFIX" -f "$ENV_FILE" --prune
else
    conda env create -p "$ENV_PREFIX" -f "$ENV_FILE"
fi

set +u
conda activate "$ENV_PREFIX"
set -u

if [[ ! -d "$WAYWALLEN_SRC/.git" ]]; then
    if [[ -e "$WAYWALLEN_SRC" ]]; then
        fail "$WAYWALLEN_SRC exists but is not a git checkout"
    fi
    info "Fetching waywallen"
    git clone https://github.com/waywallen/waywallen.git "$WAYWALLEN_SRC"
fi

info "Selecting waywallen bridge ref: $WAYWALLEN_REF"
if git -C "$WAYWALLEN_SRC" fetch --quiet origin "$WAYWALLEN_REF" 2>/dev/null; then
    git -C "$WAYWALLEN_SRC" -c advice.detachedHead=false checkout --force FETCH_HEAD
else
    git -C "$WAYWALLEN_SRC" fetch --quiet origin
    git -C "$WAYWALLEN_SRC" -c advice.detachedHead=false checkout --force "$WAYWALLEN_REF"
fi

info "Preparing shared waywallen build dependencies"
bash "$WAYWALLEN_SRC/scripts/build_ffmpeg.sh"
bash "$WAYWALLEN_SRC/scripts/copy_syslibs.sh"

SYSROOT_ARGS=()
if [[ -n "${CONDA_BUILD_SYSROOT:-}" ]]; then
    SYSROOT_ARGS=(-DCMAKE_SYSROOT="$CONDA_BUILD_SYSROOT")
fi

THREAD_ARGS=(-DCMAKE_C_FLAGS_INIT=-pthread -DCMAKE_CXX_FLAGS_INIT=-pthread)

info "Cleaning install prefix"
rm -rf "$BRIDGE_INSTALL_DIR" "$INSTALL_DIR"
mkdir -p "$BRIDGE_INSTALL_DIR" "$INSTALL_DIR" "$DIST_DIR"

info "Configuring waywallen bridge"
cmake -S "$WAYWALLEN_SRC/bridge" -B "$BRIDGE_BUILD_DIR" -G Ninja \
    -DCMAKE_BUILD_TYPE="$BUILD_TYPE" \
    -DCMAKE_C_COMPILER=clang \
    -DCMAKE_CXX_COMPILER=clang++ \
    -DCMAKE_LINKER_TYPE=LLD \
    "${SYSROOT_ARGS[@]}" \
    "${THREAD_ARGS[@]}" \
    -DCMAKE_PREFIX_PATH="$CONDA_PREFIX" \
    -DCMAKE_INSTALL_PREFIX="$BRIDGE_INSTALL_DIR" \
    -DBUILD_SHARED_LIBS=OFF

info "Building waywallen bridge"
cmake --build "$BRIDGE_BUILD_DIR" --parallel
cmake --install "$BRIDGE_BUILD_DIR"

info "Configuring open-wallpaper-engine"
cmake -S "$PROJECT_DIR" -B "$OWE_BUILD_DIR" -G Ninja \
    -DCMAKE_BUILD_TYPE="$BUILD_TYPE" \
    -DCMAKE_C_COMPILER=clang \
    -DCMAKE_CXX_COMPILER=clang++ \
    -DCMAKE_LINKER_TYPE=LLD \
    "${SYSROOT_ARGS[@]}" \
    "${THREAD_ARGS[@]}" \
    -DCMAKE_PREFIX_PATH="$BRIDGE_INSTALL_DIR;$CONDA_PREFIX" \
    -DCMAKE_INSTALL_PREFIX="$INSTALL_DIR" \
    -DOWE_WAYWALLEN_PLUGIN_BUNDLE_LAYOUT=ON \
    -DBUILD_WAYWALLEN=ON \
    -DBUILD_WEWEB=ON \
    -DBUILD_VIEWER=OFF \
    -DBUILD_TESTS=OFF

info "Building open-wallpaper-engine"
cmake --build "$OWE_BUILD_DIR" --parallel
cmake --install "$OWE_BUILD_DIR"

info "Validating plugin install"
plugin_dir="$INSTALL_DIR"
files_txt="$plugin_dir/files.txt"
test -f "$plugin_dir/plugin.toml"
test -f "$files_txt"
while IFS= read -r rel || [[ -n "$rel" ]]; do
    [[ -n "$rel" ]] || continue
    test -e "$plugin_dir/$rel"
done < "$files_txt"

info "Packaging with CPack"
rm -f "$DIST_DIR"/*.zip
cpack --config "$OWE_BUILD_DIR/CPackConfig.cmake" -B "$DIST_DIR"
shopt -s nullglob
packages=("$DIST_DIR"/*.zip)
shopt -u nullglob
[[ ${#packages[@]} -eq 1 ]] || fail "CPack did not produce exactly one zip"
package_path="${packages[0]}"

if [[ -n "${GITHUB_OUTPUT:-}" ]]; then
    output_path="$package_path"
    if [[ "$output_path" == "$PROJECT_DIR/"* ]]; then
        output_path="${output_path#$PROJECT_DIR/}"
    fi
    printf 'zip=%s\n' "$output_path" >> "$GITHUB_OUTPUT"
fi

printf '%s\n' "$package_path"
