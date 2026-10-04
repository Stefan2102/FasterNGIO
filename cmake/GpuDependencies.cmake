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
# Windows compiles the shader library (DXIL or SPIR-V) at startup with the DXC runtime. Elsewhere
# Vulkan is the only API, so the SPIR-V is compiled at build time and the executable carries no DXC.
# On Windows, FASTERNGIO_PRECOMPILED_SHADERS=ON builds the Linux arrangement (Vulkan only) to test it.
if(WIN32)
	option(FASTERNGIO_PRECOMPILED_SHADERS "Compile SPIR-V at build time instead of DXIL/SPIR-V at startup (Vulkan only)" OFF)
else()
	set(FASTERNGIO_PRECOMPILED_SHADERS ON)
endif()
if(FASTERNGIO_PRECOMPILED_SHADERS)
	set(FASTERNGIO_RUNTIME_SHADER_COMPILER OFF)
else()
	set(FASTERNGIO_RUNTIME_SHADER_COMPILER ON)
endif()
set(ORG_MODULE_SERVICES_ENABLE_DXC ${FASTERNGIO_RUNTIME_SHADER_COMPILER} CACHE BOOL "" FORCE)
# SPIR-V output for the Vulkan backend.
set(ORG_MODULE_SERVICES_ENABLE_VULKAN ${FASTERNGIO_RUNTIME_SHADER_COMPILER} CACHE BOOL "" FORCE)
set(ORG_MODULE_SERVICES_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(ORG_MODULE_SERVICES_ENABLE_ASYNC_STATE_GRAPH ON CACHE BOOL "" FORCE)

if(FASTERNGIO_RUNTIME_SHADER_COMPILER)
	# ORGModuleServices finds dxcapi.h with find_path; point it at vcpkg's directx-dxc headers.
	find_path(ORG_MODULE_SERVICES_DXC_INCLUDE_DIR dxcapi.h PATH_SUFFIXES directx-dxc REQUIRED)
endif()

set(FASTERNGIO_EXTERNAL_DIR "${PROJECT_SOURCE_DIR}/external")
add_subdirectory("${FASTERNGIO_EXTERNAL_DIR}/BasicTelemetry" "${CMAKE_BINARY_DIR}/_deps/BasicTelemetry" EXCLUDE_FROM_ALL)
add_subdirectory("${FASTERNGIO_EXTERNAL_DIR}/BasicRHI" "${CMAKE_BINARY_DIR}/_deps/BasicRHI" EXCLUDE_FROM_ALL)
add_subdirectory("${FASTERNGIO_EXTERNAL_DIR}/OpenRenderGraph" "${CMAKE_BINARY_DIR}/_deps/OpenRenderGraph" EXCLUDE_FROM_ALL)
add_subdirectory("${FASTERNGIO_EXTERNAL_DIR}/ORGModuleServices" "${CMAKE_BINARY_DIR}/_deps/ORGModuleServices" EXCLUDE_FROM_ALL)

if(FASTERNGIO_RUNTIME_SHADER_COMPILER)
	# The DXC runtime is loaded at startup by ORGModuleServices' ShaderCompiler: dxcompiler.dll and
	# the dxil.dll validator.
	find_file(FASTERNGIO_DXCOMPILER_DLL dxcompiler.dll PATH_SUFFIXES bin tools/directx-dxc REQUIRED)
	find_file(FASTERNGIO_DXIL_DLL dxil.dll PATH_SUFFIXES bin tools/directx-dxc REQUIRED)
	set(FASTERNGIO_DXC_RUNTIME "${FASTERNGIO_DXCOMPILER_DLL}" "${FASTERNGIO_DXIL_DLL}")
else()
	# Build-time SPIR-V: vcpkg's dxc by default. Its prebuilt Linux binary needs glibc 2.38; on an
	# older build machine pass another (conda-forge's directx-shader-compiler, the Vulkan SDK's) with
	# -DFASTERNGIO_DXC_EXECUTABLE=<path>. It must support -fvk-bind-resource-heap.
	find_program(FASTERNGIO_DXC_EXECUTABLE NAMES dxc PATH_SUFFIXES tools/directx-dxc
		DOC "DXC that compiles the SPIR-V shader library at build time")
	if(NOT FASTERNGIO_DXC_EXECUTABLE)
		message(FATAL_ERROR "dxc not found; install vcpkg's directx-dxc or set FASTERNGIO_DXC_EXECUTABLE")
	endif()
	execute_process(COMMAND "${FASTERNGIO_DXC_EXECUTABLE}" --version
		RESULT_VARIABLE _fasterngio_dxc_result OUTPUT_VARIABLE _fasterngio_dxc_version ERROR_VARIABLE _fasterngio_dxc_error
		OUTPUT_STRIP_TRAILING_WHITESPACE)
	if(NOT _fasterngio_dxc_result EQUAL 0)
		message(FATAL_ERROR "${FASTERNGIO_DXC_EXECUTABLE} does not run on this machine (vcpkg's needs glibc 2.38); "
			"set FASTERNGIO_DXC_EXECUTABLE to another dxc:\n${_fasterngio_dxc_error}")
	endif()
	message(STATUS "Build-time shader compiler: ${FASTERNGIO_DXC_EXECUTABLE} (${_fasterngio_dxc_version})")
	# BASICRHI_VULKAN_DXC_FLAGS: the SPIR-V ABI BasicRHI's Vulkan backend expects.
	include("${FASTERNGIO_EXTERNAL_DIR}/BasicRHI/cmake/BasicRHIShaderFlags.cmake")
endif()

# Their headers are not ours to warn about.
foreach(_fasterngio_dependency BasicRHI OpenRenderGraph ORGModuleServices BasicTelemetryCore BasicTelemetryTracy)
	if(TARGET ${_fasterngio_dependency})
		set_target_properties(${_fasterngio_dependency} PROPERTIES SYSTEM TRUE)
	endif()
endforeach()
