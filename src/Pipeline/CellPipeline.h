#pragma once

#include "GameData/GameData.h"
#include "Grass/Placement.h"
#include "Rejection/RejectionConfig.h"
#include "Rejection/WorldIndex.h"

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <stop_token>
#include <string>
#include <unordered_map>
#include <vector>

namespace FasterNGIO::Gpu
{
	class GpuRejector;
}

namespace FasterNGIO::Rejection
{
	class CpuBvh;
}

namespace FasterNGIO::Pipeline
{
	class CacheWriter;
	struct WriteTally;
}

namespace FasterNGIO::Pipeline
{
	enum class RejectionBackend
	{
		None,
		// The CPU BVH (Rejection::CpuBvh), inline on the worker that placed the cell.
		Cpu,
		// DXR / Vulkan ray tracing on the render thread.
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
		const Rejection::CpuBvh* cpuBvh{ nullptr };
		Gpu::GpuRejector* gpu{ nullptr };
		// Re-test every cell with the brute-force CPU reference and count disagreements.
		bool validateCpu{ false };
		// Cells whose candidates may wait for the GPU at once. Producers past the limit suspend
		// in the graph until a cell is written; nothing blocks.
		std::uint32_t maxCellsAwaitingGpu{ 4096 };
		// Counts cells as they finish (written, skipped, failed or cancelled), for a progress display.
		std::atomic<std::uint32_t>* progress{ nullptr };
		// Cells not yet started when a stop is requested are cancelled instead of placed.
		std::stop_token stop;
		// Finished files go to these writer threads (counted in writeTally) instead of being written by
		// the worker; producers suspend while its backlog is over budget. Null: written inline.
		CacheWriter* writer{ nullptr };
		std::shared_ptr<WriteTally> writeTally;
	};

	struct CellPipelineStats
	{
		// Written, or handed to the writer (whose WriteTally has the outcome).
		std::uint64_t cellsWritten{ 0 };
		std::uint64_t cellsSkipped{ 0 };
		std::uint64_t cellsFailed{ 0 };
		std::uint64_t cellsCancelled{ 0 };
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
