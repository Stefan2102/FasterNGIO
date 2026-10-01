# First-party GPU stack, built from the pinned submodules in external/ (sibling layout, so each
# project's own ../ fallbacks resolve to the same checkouts).

# D3D12 is Windows-only; Vulkan is the ray-tracing API everywhere else.
if(WIN32)
	set(BASICRHI_ENABLE_D3D12 ON CACHE BOOL "" FORCE)
else()
	set(BASICRHI_ENABLE_D3D12 OFF CACHE BOOL "" FORCE)
endif()
# OpenRenderGraph uses rhi::vulkan interop unconditionally, so the Vulkan backend must be built.
set(BASICRHI_ENABLE_VULKAN ON CACHE BOOL "" FORCE)
set(BASICRHI_ENABLE_STREAMLINE OFF CACHE BOOL "" FORCE)
set(BASICRHI_ENABLE_PIX OFF CACHE BOOL "" FORCE)
set(BASICRHI_ENABLE_IMGUI OFF CACHE BOOL "" FORCE)
set(BASICRHI_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(BASICRHI_SPDLOG_TARGET "spdlog::spdlog_header_only" CACHE STRING "" FORCE)
set(BASICTELEMETRY_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(BASICTELEMETRY_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
set(OPENRENDERGRAPH_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(OPENRENDERGRAPH_ENABLE_DEBUG_UI OFF CACHE BOOL "" FORCE)
set(ORG_MODULE_SERVICES_ENABLE_DXC ON CACHE BOOL "" FORCE)
# SPIR-V output for the Vulkan backend.
set(ORG_MODULE_SERVICES_ENABLE_VULKAN ON CACHE BOOL "" FORCE)
set(ORG_MODULE_SERVICES_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(ORG_MODULE_SERVICES_ENABLE_ASYNC_STATE_GRAPH ON CACHE BOOL "" FORCE)

# ORGModuleServices finds dxcapi.h with find_path; point it at vcpkg's directx-dxc headers.
find_path(ORG_MODULE_SERVICES_DXC_INCLUDE_DIR dxcapi.h PATH_SUFFIXES directx-dxc REQUIRED)

set(FASTERNGIO_EXTERNAL_DIR "${PROJECT_SOURCE_DIR}/external")
add_subdirectory("${FASTERNGIO_EXTERNAL_DIR}/BasicTelemetry" "${CMAKE_BINARY_DIR}/_deps/BasicTelemetry" EXCLUDE_FROM_ALL)
add_subdirectory("${FASTERNGIO_EXTERNAL_DIR}/BasicRHI" "${CMAKE_BINARY_DIR}/_deps/BasicRHI" EXCLUDE_FROM_ALL)
add_subdirectory("${FASTERNGIO_EXTERNAL_DIR}/OpenRenderGraph" "${CMAKE_BINARY_DIR}/_deps/OpenRenderGraph" EXCLUDE_FROM_ALL)
add_subdirectory("${FASTERNGIO_EXTERNAL_DIR}/ORGModuleServices" "${CMAKE_BINARY_DIR}/_deps/ORGModuleServices" EXCLUDE_FROM_ALL)

# The DXC runtime is loaded at startup by ORGModuleServices' ShaderCompiler: dxcompiler.dll and the
# dxil.dll validator on Windows, libdxcompiler.so on Linux (SPIR-V needs no validator).
if(WIN32)
	find_file(FASTERNGIO_DXCOMPILER_DLL dxcompiler.dll PATH_SUFFIXES bin tools/directx-dxc REQUIRED)
	find_file(FASTERNGIO_DXIL_DLL dxil.dll PATH_SUFFIXES bin tools/directx-dxc REQUIRED)
else()
	find_file(FASTERNGIO_DXCOMPILER_DLL libdxcompiler.so PATH_SUFFIXES lib tools/directx-dxc REQUIRED)
	set(FASTERNGIO_DXIL_DLL "")
endif()
set(FASTERNGIO_DXC_RUNTIME "${FASTERNGIO_DXCOMPILER_DLL}" ${FASTERNGIO_DXIL_DLL})

# Their headers are not ours to warn about.
foreach(_fasterngio_dependency BasicRHI OpenRenderGraph ORGModuleServices BasicTelemetryCore BasicTelemetryTracy)
	if(TARGET ${_fasterngio_dependency})
		set_target_properties(${_fasterngio_dependency} PROPERTIES SYSTEM TRUE)
	endif()
endforeach()
