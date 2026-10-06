add_library(FasterNGIOCore STATIC
	Archives/ArchiveResolver.cpp
	Archives/ArchiveResolver.h
	Collision/CollisionModel.h
	Collision/InstanceShape.cpp
	Collision/InstanceShape.h
	Concurrency/AtomicWait.h
	Concurrency/MpscQueue.h
	Collision/NifCollisionExtractor.cpp
	Collision/NifCollisionExtractor.h
	Collision/ObjExport.cpp
	Collision/ObjExport.h
	GameData/FormID.h
	GameData/GameData.h
	GameData/Internal/RecordExtractors.h
	GameData/Internal/RecordReader.h
	GameData/LoadOrder.cpp
	GameData/LoadOrder.h
	GameData/ModelPath.cpp
	GameData/ModelPath.h
	GameData/PluginParser.cpp
	GameData/RecordExtractors.cpp
	GameData/RecordReader.cpp
	GameData/Records.h
	GameData/StaticWorld.cpp
	GameData/StaticWorld.h
	Gpu/ModelPacking.cpp
	Gpu/ModelPacking.h
	Grass/CellCache.cpp
	Grass/CellCache.h
	Grass/GameIni.cpp
	Grass/GameIni.h
	Grass/GrassModels.cpp
	Grass/GrassModels.h
	Grass/NgioCacheWriter.cpp
	Grass/NgioCacheWriter.h
	Grass/Internal/PlacementCommon.h
	Grass/LandTexture.cpp
	Grass/LandTexture.h
	Grass/Placement.cpp
	Grass/Placement.h
	Grass/SmoothPlacement.cpp
	Grass/SmoothPlacement.h
	Grass/VanillaPlacement.cpp
	Platform/DataDirectory.cpp
	Platform/DataDirectory.h
	Platform/FileSystem.cpp
	Platform/FileSystem.h
	Platform/GameInstall.cpp
	Platform/GameInstall.h
	Platform/IniFile.cpp
	Platform/IniFile.h
	Platform/MappedFile.cpp
	Platform/MappedFile.h
	Platform/ModOrganizer.cpp
	Platform/ModOrganizer.h
	Platform/Text.cpp
	Platform/Text.h
	Platform/UserSettings.cpp
	Platform/UserSettings.h
	Platform/WholeFile.cpp
	Platform/WholeFile.h
	Rejection/Bounds.h
	Rejection/CpuBvh.cpp
	Rejection/CpuBvh.h
	Rejection/CpuReference.cpp
	Rejection/CpuReference.h
	Rejection/HlslShim.h
	Rejection/NgioRules.cpp
	Rejection/NgioRules.h
	Rejection/PrimitiveTests.h
	Rejection/RejectionConfig.cpp
	Rejection/RejectionConfig.h
	Rejection/RejectionFeatures.h
	Rejection/WorldIndex.cpp
	Rejection/WorldIndex.h
	Seasons/SeasonSwaps.cpp
	Seasons/SeasonSwaps.h
	../shaders/Shared/GrassQueryMath.hlsli
	../shaders/Shared/RejectionLayout.hlsli
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
		$<$<PLATFORM_ID:Windows>:Shell32 Ole32 Advapi32>
)

if(FASTERNGIO_ENABLE_GPU)
	add_library(FasterNGIOGpu STATIC
		Gpu/GpuRejector.cpp
		Gpu/GpuApi.h
		Gpu/GpuRejector.h
		Gpu/GpuShaders.cpp
		Gpu/GpuShaders.h
		../shaders/GrassRejection.hlsl
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
	if(FASTERNGIO_RUNTIME_SHADER_COMPILER)
		target_compile_definitions(FasterNGIOGpu PRIVATE FASTERNGIO_RUNTIME_SHADER_COMPILER=1)
	endif()
endif()

add_library(FasterNGIOPipeline STATIC
	Pipeline/CellPipeline.cpp
	Pipeline/CellPipeline.h
	Pipeline/FileWriterPool.cpp
	Pipeline/FileWriterPool.h
	Pipeline/SuspensionWaiters.cpp
	Pipeline/SuspensionWaiters.h
	Pipeline/TbbGraphScheduler.cpp
	Pipeline/TbbGraphScheduler.h
)
fasterngio_configure_target(FasterNGIOPipeline)
target_link_libraries(FasterNGIOPipeline PUBLIC FasterNGIOCore PRIVATE ORGModuleServices::AsyncStateGraph)
target_include_directories(FasterNGIOPipeline PRIVATE "${PROJECT_SOURCE_DIR}/external/ORGModuleServices/include")
if(FASTERNGIO_ENABLE_GPU)
	target_link_libraries(FasterNGIOPipeline PUBLIC FasterNGIOGpu)
endif()
