add_executable(FasterNGIO
	Main.cpp
	Generate.cpp
	Generate.h
	Gui/Launcher.cpp
	Gui/Launcher.h
	Gui/LogSink.h
	Gui/Window.h
)
# The launcher window: Win32 + D3D11 on Windows, GLFW + OpenGL 3 elsewhere (GLFW and ImGui's GL loader
# load X11 and libGL at runtime, so the command line still needs only glibc).
if(WIN32)
	target_sources(FasterNGIO PRIVATE Gui/WindowWin32.cpp)
	target_link_libraries(FasterNGIO PRIVATE d3d11 dxgi Shell32 Ole32)
else()
	target_sources(FasterNGIO PRIVATE Gui/WindowGlfw.cpp)
endif()
find_package(imgui CONFIG REQUIRED)
fasterngio_configure_target(FasterNGIO)
target_include_directories(FasterNGIO PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(FasterNGIO PRIVATE FasterNGIOCore FasterNGIOPipeline imgui::imgui)
if(FASTERNGIO_ENABLE_GPU)
	target_link_libraries(FasterNGIO PRIVATE FasterNGIOGpu)
	if(FASTERNGIO_RUNTIME_SHADER_COMPILER)
		# The shader library is compiled at startup (and cached); DXC is loaded at runtime. A separate
		# target so editing a shader alone still redeploys it (POST_BUILD only runs on relink).
		add_custom_target(FasterNGIOShaders ALL
			COMMAND ${CMAKE_COMMAND} -E copy_directory "${PROJECT_SOURCE_DIR}/shaders" "$<TARGET_FILE_DIR:FasterNGIO>/shaders"
			COMMAND ${CMAKE_COMMAND} -E copy_if_different ${FASTERNGIO_DXC_RUNTIME} "$<TARGET_FILE_DIR:FasterNGIO>"
			COMMENT "Deploying FasterNGIO shaders"
			VERBATIM)
		install(DIRECTORY "${PROJECT_SOURCE_DIR}/shaders/" DESTINATION shaders COMPONENT FasterNGIO)
		install(FILES ${FASTERNGIO_DXC_RUNTIME} DESTINATION . COMPONENT FasterNGIO)
	else()
		# Every variant GpuRejector can ask for (GpuRejector.cpp, PrecompiledShaderName), compiled with
		# the arguments ORGModuleServices' ShaderCompiler would use at runtime.
		set(_shader_source "${PROJECT_SOURCE_DIR}/shaders/GrassRejection.hlsl")
		set(_shader_output_dir "${CMAKE_CURRENT_BINARY_DIR}/shaders")
		set(_shader_outputs)
		foreach(_query ray shape)
			foreach(_debug 0 1)
				set(_name "GrassRejection.${_query}")
				if(_debug)
					string(APPEND _name ".debug")
				endif()
				set(_output "${_shader_output_dir}/${_name}.spv")
				if(_query STREQUAL "ray")
					set(_query_ray 1)
				else()
					set(_query_ray 0)
				endif()
				add_custom_command(
					OUTPUT "${_output}"
					COMMAND "${CMAKE_COMMAND}" -E make_directory "${_shader_output_dir}"
					COMMAND "${FASTERNGIO_DXC_EXECUTABLE}" -nologo -T lib_6_6 -HV 2021 -WX ${BASICRHI_VULKAN_DXC_FLAGS}
						-D QUERY_RAY=${_query_ray} -D DEBUG_QUERIES=${_debug} -I "${PROJECT_SOURCE_DIR}/shaders"
						-Fo "${_output}" "${_shader_source}"
					DEPENDS "${_shader_source}" "${PROJECT_SOURCE_DIR}/shaders/Shared/GrassQueryMath.hlsli"
					COMMENT "SPIR-V ${_name}"
					VERBATIM)
				list(APPEND _shader_outputs "${_output}")
			endforeach()
		endforeach()
		add_custom_target(FasterNGIOShaders ALL
			COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:FasterNGIO>/shaders"
			COMMAND ${CMAKE_COMMAND} -E copy_if_different ${_shader_outputs} "$<TARGET_FILE_DIR:FasterNGIO>/shaders"
			DEPENDS ${_shader_outputs}
			COMMENT "Deploying FasterNGIO shaders"
			VERBATIM)
		install(FILES ${_shader_outputs} DESTINATION shaders COMPONENT FasterNGIO)
	endif()
	add_dependencies(FasterNGIO FasterNGIOShaders)
endif()

# The release folder that build.cmd and tools/build_linux.sh fill (cmake --install --component
# FasterNGIO); a component of its own so the submodules' install rules stay out of it.
install(TARGETS FasterNGIO RUNTIME DESTINATION . COMPONENT FasterNGIO)
install(FILES "${PROJECT_SOURCE_DIR}/README.md" "${PROJECT_SOURCE_DIR}/LICENSE" DESTINATION . COMPONENT FasterNGIO)
