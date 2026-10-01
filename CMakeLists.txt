cmake_minimum_required(VERSION 3.25)
project(FasterNGIO VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(FASTERNGIO_ENABLE_GPU "Build the OpenRenderGraph/BasicRHI ray-traced rejection path" ON)
option(FASTERNGIO_WARNINGS_AS_ERRORS "Treat FasterNGIO warnings as errors" OFF)
option(FASTERNGIO_BUILD_TESTS "Build FasterNGIO unit tests" ON)

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

find_package(ZLIB REQUIRED)
find_package(lz4 CONFIG REQUIRED)
find_package(TBB CONFIG REQUIRED)
find_package(spdlog CONFIG REQUIRED)
find_package(fmt CONFIG REQUIRED)
find_package(Tracy CONFIG REQUIRED)

add_subdirectory(cmake/nifly)

if(FASTERNGIO_ENABLE_GPU)
	include(cmake/GpuDependencies.cmake)
else()
	# The CPU pipeline still runs on ORGModuleServices' AsyncStateGraph.
	set(BASICTELEMETRY_BUILD_TESTS OFF CACHE BOOL "" FORCE)
	set(BASICTELEMETRY_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
	set(BASICTELEMETRY_BUILD_ARTIFACTS OFF CACHE BOOL "" FORCE)
	add_subdirectory("${PROJECT_SOURCE_DIR}/external/BasicTelemetry" "${CMAKE_BINARY_DIR}/_deps/BasicTelemetry" EXCLUDE_FROM_ALL)
	include("${PROJECT_SOURCE_DIR}/external/ORGModuleServices/cmake/AsyncStateGraph.cmake")
endif()

function(fasterngio_configure_target target)
	target_compile_features(${target} PUBLIC cxx_std_23)
	target_compile_definitions(${target} PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS)
	if(MSVC)
		target_compile_options(${target} PRIVATE /W4 /utf-8 /permissive- /Zc:__cplusplus)
		if(FASTERNGIO_WARNINGS_AS_ERRORS)
			target_compile_options(${target} PRIVATE /WX)
		endif()
	endif()
endfunction()

add_subdirectory(src)
add_subdirectory(apps/FasterNGIO)

if(FASTERNGIO_BUILD_TESTS)
	enable_testing()
	add_subdirectory(tests)
endif()
