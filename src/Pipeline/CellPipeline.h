#pragma once

#include "GameData/GameData.h"
#include "Grass/CellCache.h"
#include "Grass/Placement.h"
#include "Rejection/RejectionConfig.h"
#include "Rejection/RejectionFeatures.h"
#include "Rejection/WorldIndex.h"

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <stop_token>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace FasterNGIO::Gpu
{
	class GpuRejector;
}

namespace FasterNGIO::Grass
{
	class LandTextureMask;
}

namespace FasterNGIO::Rejection
{
	class CpuBvh;
}

namespace FasterNGIO::Pipeline
{
	class FileWriterPool;
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
		// Each cell's file is written under every one of these names: "" for the plain cache, a
		// season's suffix ("WIN") for Grass Cache Helper NG's. A cell is skipped only when all exist.
		std::vector<std::string> fileSuffixes{ std::string{} };
		Grass::PlacementSettings placement;
		// Blades per block and the quadrant cap (Grass::FinalizeCell).
		Grass::BlockLayout blockLayout;
		bool overwrite{ false };
		// A cell left with no grass gets no file (with overwrite, an existing one is removed) instead of
		// NGIO's 4-byte empty cache.
		bool skipEmpty{ false };
		// The .cgid names (lower case) in outputDirectory when the run started: an empty cell removes
		// only these, rather than trying every name. Null: every name is tried.
		const std::unordered_set<std::string>* existingFiles{ nullptr };
		const std::unordered_map<GameData::FormID, Rejection::QueryShape, GameData::FormIDHash>* shapesByGrass{ nullptr };
		RejectionBackend backend{ RejectionBackend::None };
		const Rejection::WorldIndex* world{ nullptr };
		const Rejection::CpuBvh* cpuBvh{ nullptr };
		Gpu::GpuRejector* gpu{ nullptr };
		// Re-test every cell with the brute-force CPU reference and count disagreements.
		bool validateCpu{ false };
		// NGIO's per-blade filters, applied with rejection: grass types that are never rejected
		// (Ray-cast-ignore-grass-forms), and land textures that reject grass on them or within
		// textureWidth along x and y (Ray-cast-texture-forms).
		const Rejection::FormSet* ignoredGrass{ nullptr };
		const Grass::LandTextureMask* textureMask{ nullptr };
		float textureWidth{ 0.0f };
		// NGIO's cliffs and ignored shapes (the instances' roles in world say which apply).
		const Rejection::RejectionFeatures* features{ nullptr };
		// Counts cells as they finish (written, skipped, failed or cancelled), for a progress display.
		std::atomic<std::uint32_t>* progress{ nullptr };
		// Cells not yet started when a stop is requested are cancelled instead of placed.
		std::stop_token stop;
		// Finished files go to these writer threads (counted in writeTally) instead of being written by
		// the worker; producers suspend while its backlog is over budget. Null: written inline.
		FileWriterPool* writer{ nullptr };
		std::shared_ptr<WriteTally> writeTally;
	};

	struct CellPipelineStats
	{
		// Written, or handed to the writer (whose WriteTally has the outcome).
		std::uint64_t cellsWritten{ 0 };
		std::uint64_t cellsSkipped{ 0 };
		// Cells with no grass whose files were not written (skipEmpty).
		std::uint64_t cellsEmpty{ 0 };
		std::uint64_t cellsFailed{ 0 };
		std::uint64_t cellsCancelled{ 0 };
		std::uint64_t blades{ 0 };
		std::uint64_t bladesRejected{ 0 };
		std::uint64_t validationMismatches{ 0 };
		// Blades NGIO's grass cliffs moved onto a cliff.
		std::uint64_t bladesMoved{ 0 };
		// Blades thinned away by the engine's per-quadrant cap.
		std::uint64_t bladesCapped{ 0 };
	};

	// Places, rejects and writes every cell through an AsyncStateGraph: a CellTrace artifact per
	// cell (placement, then a GPU trace job posted lock-free to the render thread) and a CellOutput
	// artifact that requires it at GpuReady (finalize + .cgid). The calling thread only posts the
	// requests and then sleeps on a counter until every cell has been written.
	[[nodiscard]] CellPipelineStats RunCellPipeline(const CellPipelineDesc& a_desc);
}
