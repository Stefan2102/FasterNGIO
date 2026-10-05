#!/usr/bin/env bash
# Installs what the Linux build needs; tools/build_linux.sh runs it when something is missing.
#
#   tools/setup_linux.sh [packages|deps|all]
#
#   packages  the compiler (GCC 14+), cmake, ninja, the Vulkan loader, the X11 and GL headers and
#             vcpkg's tools, through the system package manager (apt; sudo unless already root). Skipped
#             when they are already there.
#   deps      a vcpkg checkout and Vulkan-Headers (VULKAN_HEADERS_TAG) under FASTERNGIO_DEPS_DIR
#             (default ~/.local/share/fasterngio), as the user. build_linux.sh uses them when
#             VCPKG_ROOT / VULKAN_HEADERS_DIR are not set.
#   all       both (the default).
#
# build.cmd runs "packages" as root in WSL (wsl -u root, no password prompt) and then the build as
# the default user.
set -euo pipefail

step=${1:-all}
case "$step" in
	packages|deps|all) ;;
	*) echo "usage: $0 [packages|deps|all]" >&2; exit 2 ;;
esac

source_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
deps_dir=${FASTERNGIO_DEPS_DIR:-$HOME/.local/share/fasterngio}
# VK_EXT_descriptor_heap first shipped in Vulkan SDK 1.4.357.
vulkan_headers_tag=${VULKAN_HEADERS_TAG:-v1.4.357}

fail() { echo "error: $*" >&2; exit 1; }

# GCC 14 is the oldest with the C++23 the sources use.
gcc_ok() {
	command -v g++ >/dev/null || return 1
	local major
	major=$(g++ -dumpversion | cut -d. -f1)
	(( major >= 14 ))
}

packages_present() {
	local tool
	for tool in cmake ninja git curl zip unzip tar pkg-config; do
		command -v "$tool" >/dev/null || return 1
	done
	# FindVulkan (BasicRHI) insists on the loader library, though volk loads the driver at runtime.
	ldconfig -p 2>/dev/null | grep -q "libvulkan.so " || return 1
	# GLFW and ImGui (the launcher window) build against the X11 and GL headers; the libraries are
	# loaded at runtime.
	pkg-config --exists x11 xrandr xinerama xcursor xi xext gl || return 1
	gcc_ok
}

install_packages() {
	if packages_present; then
		return
	fi
	local sudo=
	if (( EUID != 0 )); then
		command -v sudo >/dev/null || fail "run this as root to install the build tools"
		sudo=sudo
	fi
	if command -v apt-get >/dev/null; then
		echo "=== Installing build tools (apt) ==="
		$sudo env DEBIAN_FRONTEND=noninteractive apt-get update
		$sudo env DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
			build-essential cmake ninja-build git curl ca-certificates zip unzip tar pkg-config libvulkan-dev \
			libx11-dev libxrandr-dev libxinerama-dev libxcursor-dev libxi-dev libxext-dev libgl-dev
		if ! gcc_ok; then
			# Older releases (Ubuntu 22.04, Debian 12) default to an older GCC but may package 14.
			$sudo env DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends g++-14 ||
				fail "GCC 14 or newer is required and this distribution's default is older"
			echo "note: set CC=gcc-14 CXX=g++-14 (build-linux.env) to build with it"
		fi
	elif command -v dnf >/dev/null; then
		$sudo dnf install -y gcc-c++ cmake ninja-build git curl zip unzip tar pkgconf-pkg-config perl vulkan-loader-devel \
			libX11-devel libXrandr-devel libXinerama-devel libXcursor-devel libXi-devel libXext-devel mesa-libGL-devel
	elif command -v pacman >/dev/null; then
		$sudo pacman -S --needed --noconfirm base-devel cmake ninja git curl zip unzip tar pkgconf vulkan-icd-loader \
			libx11 libxrandr libxinerama libxcursor libxi libxext mesa
	else
		fail "no supported package manager; install GCC 14+, cmake 3.25+, ninja, git, curl, zip, unzip, tar, pkg-config, the Vulkan loader and the X11 and GL headers"
	fi
}

install_deps() {
	command -v git >/dev/null || fail "git not found; run $0 packages first"
	mkdir -p "$deps_dir"

	local vcpkg_dir=$deps_dir/vcpkg
	if [[ ! -d $vcpkg_dir/.git ]]; then
		echo "=== Cloning vcpkg into $vcpkg_dir ==="
		git clone https://github.com/microsoft/vcpkg.git "$vcpkg_dir"
	fi
	# Manifest mode needs the commit vcpkg.json pins as its baseline.
	local baseline
	baseline=$(sed -n 's/.*"builtin-baseline"[[:space:]]*:[[:space:]]*"\([0-9a-f]*\)".*/\1/p' "$source_dir/vcpkg.json")
	if [[ -n $baseline ]] && ! git -C "$vcpkg_dir" cat-file -e "$baseline^{commit}" 2>/dev/null; then
		git -C "$vcpkg_dir" fetch origin
	fi
	if [[ ! -x $vcpkg_dir/vcpkg ]]; then
		"$vcpkg_dir/bootstrap-vcpkg.sh" -disableMetrics
	fi

	local headers_dir=$deps_dir/Vulkan-Headers-$vulkan_headers_tag
	if [[ ! -d $headers_dir/include/vulkan ]]; then
		echo "=== Fetching Vulkan-Headers $vulkan_headers_tag ==="
		rm -rf "$headers_dir"
		git clone --depth 1 --branch "$vulkan_headers_tag" https://github.com/KhronosGroup/Vulkan-Headers.git "$headers_dir"
	fi
}

if [[ $step == packages || $step == all ]]; then
	install_packages
fi
if [[ $step == deps || $step == all ]]; then
	install_deps
fi
