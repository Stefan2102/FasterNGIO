#!/usr/bin/env bash
# Configures and builds the Linux version, natively or under WSL (build.cmd runs it there).
#
#   tools/build_linux.sh [gpu|cpu] [--test]
#
# Environment:
#   VCPKG_ROOT                  vcpkg checkout (required).
#   VULKAN_HEADERS_DIR          Vulkan-Headers include directory with VK_EXT_descriptor_heap
#                               (Vulkan SDK 1.4.357 or the matching Vulkan-Headers tag). GPU only.
#   FASTERNGIO_DXC_EXECUTABLE   dxc for the build-time SPIR-V compile. Default: vcpkg's, which needs
#                               glibc 2.38 (Debian 13, Ubuntu 24.04); older machines can use
#                               conda-forge's directx-shader-compiler or the Vulkan SDK's. GPU only.
#   FASTERNGIO_BUILD_DIR        Build directory. Default build/linux[-cpu] in the repository. Under
#                               WSL a directory on the Linux filesystem builds several times faster.
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
	gpu=ON
	features=gpu
else
	suffix=-cpu
	gpu=OFF
	features=
fi
# Sourced after the variant is known: it may use $variant and $suffix.
if [[ -f "$source_dir/build-linux.env" ]]; then
	# shellcheck source=/dev/null
	source "$source_dir/build-linux.env"
fi
build_dir=${FASTERNGIO_BUILD_DIR:-$source_dir/build/linux$suffix}

fail() { echo "error: $*" >&2; exit 1; }
for tool in cmake ninja; do
	command -v "$tool" >/dev/null || fail "$tool not found on PATH"
done
[[ -n ${VCPKG_ROOT:-} && -f "$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake" ]] || fail "set VCPKG_ROOT to a vcpkg checkout"

configure=(
	-S "$source_dir" -B "$build_dir" -G Ninja
	-DCMAKE_BUILD_TYPE=RelWithDebInfo
	"-DCMAKE_TOOLCHAIN_FILE=$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake"
	-DVCPKG_TARGET_TRIPLET=x64-linux
	"-DVCPKG_MANIFEST_FEATURES=$features"
	"-DFASTERNGIO_ENABLE_GPU=$gpu"
)
if [[ $variant == gpu ]]; then
	[[ -n ${VULKAN_HEADERS_DIR:-} && -d "$VULKAN_HEADERS_DIR/vulkan" ]] ||
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
