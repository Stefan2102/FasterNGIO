# Agility uses the system d3d12.dll loader (and its import library), which selects D3D12Core.dll
# using exports from the process executable. BasicRHI's d3d12 link is therefore still required.
if(CMAKE_GENERATOR MATCHES "^Visual Studio")
	# Visual Studio supplies the Windows SDK library search path at build time, outside CMake's
	# find_library search. The vcpkg config accepts the import library name in that case.
	set(D3D12_LIB d3d12.lib)
endif()
find_package(directx-headers CONFIG REQUIRED)
find_package(directx12-agility CONFIG REQUIRED)

# Keep the core and its matching debug layer together, separate from the executable. Deploy the
# layer in release builds too: --gpu-debug is a runtime choice, independent of the build type.
set(FASTERNGIO_AGILITY_RUNTIME
	"$<TARGET_FILE:Microsoft::DirectX12-Core>"
	"$<TARGET_FILE:Microsoft::DirectX12-Layers>")
add_custom_target(FasterNGIOAgilityRuntime ALL
	COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:FasterNGIO>/D3D12"
	COMMAND ${CMAKE_COMMAND} -E copy_if_different ${FASTERNGIO_AGILITY_RUNTIME} "$<TARGET_FILE_DIR:FasterNGIO>/D3D12"
	COMMENT "Deploying DirectX 12 Agility SDK"
	VERBATIM)
install(FILES ${FASTERNGIO_AGILITY_RUNTIME} DESTINATION D3D12 COMPONENT FasterNGIO)
install(FILES "${directx12-agility_DIR}/copyright" DESTINATION D3D12 RENAME LICENSE.txt COMPONENT FasterNGIO)

# Each executable hosting BasicRHI needs the exports itself, including the test runner. A static
# library would let the linker discard this translation unit because nothing references it.
function(fasterngio_enable_agility target)
	target_sources(${target} PRIVATE "${PROJECT_SOURCE_DIR}/src/Gpu/AgilitySdk.cpp")
	target_link_libraries(${target} PRIVATE Microsoft::DirectX-Headers Microsoft::DirectX12-Agility)
	add_dependencies(${target} FasterNGIOAgilityRuntime)
endfunction()
