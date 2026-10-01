add_executable(FasterNGIO Main.cpp)
fasterngio_configure_target(FasterNGIO)
target_link_libraries(FasterNGIO PRIVATE FasterNGIOCore FasterNGIOPipeline)
if(FASTERNGIO_ENABLE_GPU)
	target_link_libraries(FasterNGIO PRIVATE FasterNGIOGpu)
	# The shader library is compiled at startup (and cached); DXC is loaded at runtime. A separate
	# target so editing a shader alone still redeploys it (POST_BUILD only runs on relink).
	add_custom_target(FasterNGIOShaders ALL
		COMMAND ${CMAKE_COMMAND} -E copy_directory "${PROJECT_SOURCE_DIR}/shaders" "$<TARGET_FILE_DIR:FasterNGIO>/shaders"
		COMMAND ${CMAKE_COMMAND} -E copy_if_different ${FASTERNGIO_DXC_RUNTIME} "$<TARGET_FILE_DIR:FasterNGIO>"
		COMMENT "Deploying FasterNGIO shaders"
		VERBATIM)
	add_dependencies(FasterNGIO FasterNGIOShaders)
endif()
