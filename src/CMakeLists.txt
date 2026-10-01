add_library(FasterNGIOCore STATIC
	Archives/ArchiveResolver.cpp
	Archives/ArchiveResolver.h
	Collision/CollisionModel.h
	Collision/NifCollisionExtractor.cpp
	Collision/NifCollisionExtractor.h
	Collision/ObjExport.cpp
	Collision/ObjExport.h
	GameData/GameData.cpp
	GameData/GameData.h
	GameData/ModelPathNormalization.cpp
	GameData/PluginFileReader.cpp
	GameData/Internal/PluginFileReader.h
	Grass/GameIni.cpp
	Grass/GameIni.h
	Grass/NgioCacheWriter.cpp
	Grass/NgioCacheWriter.h
	Grass/Placement.cpp
	Grass/Placement.h
	Platform/DataDirectory.cpp
	Platform/DataDirectory.h
	Platform/IniFile.cpp
	Platform/IniFile.h
	Rejection/CpuBvh.cpp
	Rejection/CpuBvh.h
	Rejection/CpuReference.cpp
	Rejection/CpuReference.h
	Rejection/HlslShim.h
	Rejection/PrimitiveTests.h
	Rejection/RejectionConfig.cpp
	Rejection/RejectionConfig.h
	Rejection/WorldIndex.cpp
	Rejection/WorldIndex.h
	../shaders/Shared/GrassQueryMath.hlsli
)
fasterngio_configure_target(FasterNGIOCore)
target_include_directories(FasterNGIOCore PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(FasterNGIOCore
	PUBLIC
		TBB::tbb
		spdlog::spdlog_header_only
		nifly::nifly
	PRIVATE
		ZLIB::ZLIB
		lz4::lz4
		$<$<PLATFORM_ID:Windows>:Shell32 Ole32>
)

if(FASTERNGIO_ENABLE_GPU)
	add_library(FasterNGIOGpu STATIC
		Gpu/GpuRejector.cpp
		Gpu/GpuRejector.h
		../shaders/GrassRejection.hlsl
		Pipeline/MpscQueue.h
	)
	set_source_files_properties(../shaders/GrassRejection.hlsl PROPERTIES HEADER_FILE_ONLY TRUE)
	fasterngio_configure_target(FasterNGIOGpu)
	target_link_libraries(FasterNGIOGpu
		PUBLIC
			FasterNGIOCore
		PRIVATE
			OpenRenderGraph::OpenRenderGraph
			BasicRHI::BasicRHI
			ORGModuleServices::ORGModuleServices
	)
	target_compile_definitions(FasterNGIOGpu PUBLIC FASTERNGIO_HAS_GPU=1)
endif()

add_library(FasterNGIOPipeline STATIC
	Pipeline/CellPipeline.cpp
	Pipeline/CellPipeline.h
	Pipeline/MpscQueue.h
	Pipeline/TbbGraphScheduler.cpp
	Pipeline/TbbGraphScheduler.h
)
fasterngio_configure_target(FasterNGIOPipeline)
target_link_libraries(FasterNGIOPipeline PUBLIC FasterNGIOCore PRIVATE ORGModuleServices::AsyncStateGraph)
target_include_directories(FasterNGIOPipeline PRIVATE "${PROJECT_SOURCE_DIR}/external/ORGModuleServices/include")
if(FASTERNGIO_ENABLE_GPU)
	target_link_libraries(FasterNGIOPipeline PUBLIC FasterNGIOGpu)
endif()
