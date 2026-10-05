# Settings shared by setup_linux.sh and build_linux.sh (sourced, not run).

# Where setup_linux.sh puts vcpkg and Vulkan-Headers.
deps_dir=${FASTERNGIO_DEPS_DIR:-$HOME/.local/share/fasterngio}
# VK_EXT_descriptor_heap first shipped in Vulkan SDK 1.4.357.
vulkan_headers_tag=${VULKAN_HEADERS_TAG:-v1.4.357}
vulkan_headers_dir=$deps_dir/Vulkan-Headers-$vulkan_headers_tag

fail() { echo "error: $*" >&2; exit 1; }
