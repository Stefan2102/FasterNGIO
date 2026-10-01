#pragma once

#include "GameData/GameData.h"
#include "Grass/Placement.h"
#include "Rejection/RejectionConfig.h"
#include "Rejection/WorldIndex.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

namespace FasterNGIO::Gpu
{
	class GpuRejector;
}

namespace FasterNGIO::Pipeline
{
	enum class RejectionBackend
	{
		None,
		Cpu,
		Gpu
	};

	struct CellPipelineDesc
	{
		const GameData::StaticWorldSnapshot* snapshot{ nullptr };
		std::vector<const GameData::LandInfo*> lands;
		std::string worldEditorID;
		std::filesystem::path outputDirectory;
		Grass::PlacementSettings placement;
		bool overwrite{ false };
		const std::unordered_map<GameData::FormID, Rejection::QueryShape, GameData::FormIDHash>* shapesByGrass{ nullptr };
		RejectionBackend backend{ RejectionBackend::None };
		const Rejection::WorldIndex* world{ nullptr };
		Gpu::GpuRejector* gpu{ nullptr };
		bool validateCpu{ false };
		// Cells whose candidates may wait for the GPU at once. Producers past the limit suspend
		// in the graph until a cell is written; nothing blocks.
		std::uint32_t maxCellsAwaitingGpu{ 4096 };
	};

	struct CellPipelineStats
	{
		std::uint64_t cellsWritten{ 0 };
		std::uint64_t cellsSkipped{ 0 };
		std::uint64_t cellsFailed{ 0 };
		std::uint64_t blades{ 0 };
		std::uint64_t bladesRejected{ 0 };
		std::uint64_t validationMismatches{ 0 };
	};

	// Places, rejects and writes every cell through an AsyncStateGraph: a CellTrace artifact per
	// cell (placement, then a GPU trace job posted lock-free to the render thread) and a CellOutput
	// artifact that requires it at GpuReady (finalize + .cgid). The calling thread only posts the
	// requests and then sleeps on a counter until every cell has been written.
	[[nodiscard]] CellPipelineStats RunCellPipeline(const CellPipelineDesc& a_desc);
}
