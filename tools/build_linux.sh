#!/usr/bin/env bash
# Configures, builds and packages the Linux version, natively or under WSL (build.cmd runs it there).
#
#   tools/build_linux.sh [gpu|cpu] [--test]
#
# The release folder (the executable, its shaders, README and LICENSE) goes to
# release/FasterNGIO-linux[-cpu] in the repository, or FASTERNGIO_RELEASE_DIR.
#
# Missing build tools are installed with tools/setup_linux.sh (apt through sudo), and so are vcpkg
# and Vulkan-Headers when VCPKG_ROOT / VULKAN_HEADERS_DIR are not set.
#
# Environment:
#   VCPKG_ROOT                  vcpkg checkout. Default: setup_linux.sh's, in FASTERNGIO_DEPS_DIR.
#   VULKAN_HEADERS_DIR          Vulkan-Headers include directory with VK_EXT_descriptor_heap
#                               (Vulkan SDK 1.4.357 or the matching Vulkan-Headers tag). GPU only.
#                               Default: setup_linux.sh's.
#   VULKAN_HEADERS_TAG          The Vulkan-Headers tag setup_linux.sh fetches (tools/linux_common.sh).
#   FASTERNGIO_DEPS_DIR         Where setup_linux.sh puts vcpkg and Vulkan-Headers. Default
#                               ~/.local/share/fasterngio.
#   FASTERNGIO_DXC_EXECUTABLE   dxc for the build-time SPIR-V compile. Default: vcpkg's, which needs
#                               glibc 2.38 (Debian 13, Ubuntu 24.04); older machines can use
#                               conda-forge's directx-shader-compiler or the Vulkan SDK's. GPU only.
#   FASTERNGIO_BUILD_DIR        Build directory. Default build/linux[-cpu] in the repository, or,
#                               under WSL with the repository on a Windows drive, ~/.cache/fasterngio/
#                               build/linux[-cpu] (the Linux filesystem builds several times faster).
#   FASTERNGIO_RELEASE_DIR      Release folder. Default release/FasterNGIO-linux[-cpu].
#   CC, CXX, PATH               The compiler (C++23: GCC 14 or newer), cmake (3.25+) and ninja.
#
# build-linux.env in the repository root (untracked) is sourced when it exists, for machine-specific
# values; it can use $variant (gpu or cpu) and $suffix (empty or -cpu).
set -euo pipefail

variant=gpu
run_tests=0
for arg in "$@"; do
	case "$arg" in
		gpu|cpu) variant=$arg ;;
		--test) run_tests=1 ;;
		*) echo "usage: $0 [gpu|cpu] [--test]" >&2; exit 2 ;;
	esac
done

source_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
if [[ $variant == gpu ]]; then
	suffix=
else
	suffix=-cpu
fi
# The configure preset (CMakePresets.json): linux or linux-cpu.
preset=linux$suffix
# Sourced after the variant is known: it may use $variant and $suffix.
if [[ -f "$source_dir/build-linux.env" ]]; then
	# shellcheck source=/dev/null
	source "$source_dir/build-linux.env"
fi
if [[ -z ${FASTERNGIO_BUILD_DIR:-} && $source_dir == /mnt/* ]] && grep -qi microsoft /proc/version 2>/dev/null; then
	FASTERNGIO_BUILD_DIR=$HOME/.cache/fasterngio/build/linux$suffix
fi
build_dir=${FASTERNGIO_BUILD_DIR:-$source_dir/build/linux$suffix}
release_dir=${FASTERNGIO_RELEASE_DIR:-$source_dir/release/FasterNGIO-linux$suffix}
# shellcheck source=linux_common.sh
source "$source_dir/tools/linux_common.sh"
export FASTERNGIO_DEPS_DIR=$deps_dir

setup=$source_dir/tools/setup_linux.sh
# Installs only what is missing (through sudo); a no-op once everything is there.
bash "$setup" packages
for tool in cmake ninja; do
	command -v "$tool" >/dev/null || fail "$tool not found on PATH"
done

# vcpkg and Vulkan-Headers: the caller's, else setup_linux.sh's.
if [[ -z ${VCPKG_ROOT:-} ]]; then
	VCPKG_ROOT=$deps_dir/vcpkg
	[[ -x $VCPKG_ROOT/vcpkg ]] || bash "$setup" deps
fi
if [[ $variant == gpu && -z ${VULKAN_HEADERS_DIR:-} ]]; then
	VULKAN_HEADERS_DIR=$vulkan_headers_dir/include
	[[ -d $VULKAN_HEADERS_DIR/vulkan ]] || bash "$setup" deps
fi
[[ -f "$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake" ]] || fail "set VCPKG_ROOT to a vcpkg checkout"
# The preset finds the toolchain through it.
export VCPKG_ROOT

# The preset has the generator, build type, triplet and GPU options; only the directory and the
# machine's paths are added here.
configure=(-S "$source_dir" --preset "$preset" -B "$build_dir")
if [[ $variant == gpu ]]; then
	[[ -d "$VULKAN_HEADERS_DIR/vulkan" ]] ||
		fail "set VULKAN_HEADERS_DIR to a Vulkan-Headers include directory with VK_EXT_descriptor_heap"
	configure+=("-DVulkan_INCLUDE_DIR=$VULKAN_HEADERS_DIR")
	if [[ -n ${FASTERNGIO_DXC_EXECUTABLE:-} ]]; then
		configure+=("-DFASTERNGIO_DXC_EXECUTABLE=$FASTERNGIO_DXC_EXECUTABLE")
	fi
fi

cmake "${configure[@]}"
cmake --build "$build_dir"
if (( run_tests )); then
	ctest --test-dir "$build_dir" --output-on-failure
fi
echo "Linux ($variant) build: $build_dir/bin"

# Stripped: RelWithDebInfo's debug info is most of the executable's size.
rm -rf "$release_dir"
cmake --install "$build_dir" --component FasterNGIO --prefix "$release_dir" --strip
echo "Linux ($variant) release: $release_dir"
