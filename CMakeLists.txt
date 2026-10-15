cmake_minimum_required(VERSION 3.28)
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

if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
	# The C++ runtime goes into the executable, so it needs only glibc and runs on distributions
	# whose libstdc++ is older than the compiler that built it.
	add_link_options(-static-libstdc++ -static-libgcc)
endif()

# Only TESWorld core is needed: plugin decoding and world resolution, without a renderer or assets.
set(FASTERNGIO_TESWORLD_SOURCE_DIR "${PROJECT_SOURCE_DIR}/external/tesworld" CACHE PATH "TESWorld submodule or development checkout")
if(NOT EXISTS "${FASTERNGIO_TESWORLD_SOURCE_DIR}/CMakeLists.txt")
    message(FATAL_ERROR "TESWorld is missing: run git submodule update --init external/tesworld")
endif()
foreach(feature VFS TEXTURE ATLAS NIF RENDER GPU PROFILING)
    set(TESWORLD_WITH_${feature} OFF CACHE BOOL "" FORCE)
endforeach()
set(TESWORLD_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(TESWORLD_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
add_subdirectory("${FASTERNGIO_TESWORLD_SOURCE_DIR}" "${CMAKE_BINARY_DIR}/tesworld" EXCLUDE_FROM_ALL)

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
	if(WIN32)
		target_compile_definitions(${target} PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS)
	endif()
	if(MSVC)
		target_compile_options(${target} PRIVATE /W4 /utf-8 /permissive- /Zc:__cplusplus)
		if(FASTERNGIO_WARNINGS_AS_ERRORS)
			target_compile_options(${target} PRIVATE /WX)
		endif()
	else()
		target_compile_options(${target} PRIVATE -Wall -Wextra)
		if(FASTERNGIO_WARNINGS_AS_ERRORS)
			target_compile_options(${target} PRIVATE -Werror)
		endif()
	endif()
endfunction()

add_subdirectory(src)
add_subdirectory(apps/FasterNGIO)

if(FASTERNGIO_BUILD_TESTS)
	enable_testing()
	add_subdirectory(tests)
endif()
