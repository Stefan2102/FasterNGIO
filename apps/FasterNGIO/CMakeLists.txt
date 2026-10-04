add_executable(FasterNGIO Main.cpp)
fasterngio_configure_target(FasterNGIO)
target_link_libraries(FasterNGIO PRIVATE FasterNGIOCore FasterNGIOPipeline)
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
	endif()
	add_dependencies(FasterNGIO FasterNGIOShaders)
endif()
