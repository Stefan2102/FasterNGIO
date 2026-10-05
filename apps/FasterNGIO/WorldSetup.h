#pragma once

#include "Generate.h"

#include "Archives/ArchiveResolver.h"
#include "Grass/SmoothPlacement.h"
#include "Rejection/RejectionConfig.h"
#include "Rejection/WorldIndex.h"

#include <chrono>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#if FASTERNGIO_HAS_GPU
#include "Gpu/GpuRejector.h"
#endif

// Preparing one worldspace for placement and rejection: shared by generation (Generate.cpp) and
// the diagnostics (Diagnostics.cpp).
namespace FasterNGIO::App
{
	[[nodiscard]] double SecondsSince(std::chrono::steady_clock::time_point a_begin);

	// The query volume of each grass type.
	using ShapeMap = std::unordered_map<GameData::FormID, Rejection::QueryShape, GameData::FormIDHash>;

	struct QueryShapes
	{
		ShapeMap byGrass;
		// The widest radius of any type: collision bounds are grown by it.
		float maxReach{ 0.0f };
	};

	[[nodiscard]] QueryShapes MakeQueryShapes(const GameData::StaticWorldSnapshot& a_snapshot, const Rejection::RejectionConfig& a_config);

	// Engine defaults, then the game's INIs (as the game would read them), then the options' overrides.
	[[nodiscard]] Grass::PlacementSettings ResolvePlacementSettings(const GenerateOptions& a_options);

	// Whether a cell is within --cell or --radius (every cell when neither is given).
	[[nodiscard]] bool IsSelectedCell(const GenerateOptions& a_options, std::int32_t a_x, std::int32_t a_y);

	// The worldspace's LANDs within the selected cells (--cell, --radius), one per cell (the first
	// seen wins), in row order. Throws when the worldspace has no LAND.
	[[nodiscard]] std::vector<const GameData::LandInfo*> SelectLands(const GameData::StaticWorldSnapshot& a_snapshot, GameData::FormID a_worldFormID,
		const GenerateOptions& a_options);

	// The placement settings for one worldspace: with smooth placement, its weight field too, built
	// once and read by every worker.
	struct WorldPlacement
	{
		Grass::PlacementSettings settings;
		std::unique_ptr<Grass::SmoothWeightField> field;
	};

	[[nodiscard]] WorldPlacement PrepareWorldPlacement(const GameData::StaticWorldSnapshot& a_snapshot, GameData::FormID a_worldFormID, const Grass::PlacementSettings& a_settings);

	// The archives in the load order's resource order, over the options' Data folder.
	[[nodiscard]] Archives::ArchiveResolver MakeResolver(const GenerateOptions& a_options, const LoadedPlugins& a_plugins);

	// Every rejecting instance of the worldspace, extracted from its models' collision.
	[[nodiscard]] std::shared_ptr<const Rejection::WorldIndex> BuildWorldIndex(const GameData::StaticWorldSnapshot& a_snapshot, GameData::FormID a_worldFormID,
		const Archives::ArchiveResolver& a_resolver, const Rejection::RejectionFeatures& a_features, float a_maxReach);

#if FASTERNGIO_HAS_GPU
	// Creates the GPU rejector for --reject auto or gpu. Throws (GpuUnsupportedError when the adapter
	// lacks something the ray-tracing path needs).
	[[nodiscard]] std::unique_ptr<Gpu::GpuRejector> CreateGpuRejector(const GenerateOptions& a_options, const QueryShapes& a_shapes);
#endif
}
