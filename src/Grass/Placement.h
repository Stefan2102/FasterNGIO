#pragma once

#include "GameData/GameData.h"
#include "Grass/NgioCacheWriter.h"

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace FasterNGIO::Grass
{
	enum class PlacementMode
	{
		// The engine's algorithm, bit for bit (RNG order included).
		Vanilla,
		// Density evaluated per blade from a continuous texture-weight field (the tool's default).
		Smooth
	};

	class SmoothWeightField;

	// Smooth placement: each blade's acceptance comes from the texture weights at its own
	// (optionally warped) position, interpolated bilinearly from vertices merged across quadrants
	// and cells, instead of vanilla's on/off density per 512-unit patch. Vanilla's 512-unit patch
	// grid, quadrant and cell seams and on/off steps all disappear (see AGENTS.md, Placement).
	struct SmoothPlacementSettings
	{
		// Coverage ramps (smoothstep) from none at coverageLow texture weight to full at
		// coverageHigh; fTexturePctThreshold raises coverageLow when it is higher.
		float coverageLow{ 0.05f };
		float coverageHigh{ 0.5f };
		// Coverage is multiplied by densityBias and capped at 1, which shifts grass from texture
		// cores into blends: 1 makes density proportional to weight; larger values approach vanilla's
		// "any weight is full density".
		float densityBias{ 1.6f };
		// Scale each grass type's density so its expected blade count over the whole worldspace
		// matches vanilla placement's. Vanilla places a grass once per texture that carries it, so
		// blends of such textures double it; this keeps the total without reproducing those bands.
		bool matchVanillaDensity{ true };
		// Low-frequency domain warp of the weight lookup, in game units; 0 disables. Breaks up the
		// 128-unit vertex grid that bilinear contours otherwise follow.
		float warpAmplitude{ 96.0f };
		float warpWavelength{ 512.0f };
		// Cached weights for the cell and its neighbours. Without one, each cell's weights are built
		// on the fly and its border vertices are not merged with the neighbouring cells'.
		const SmoothWeightField* field{ nullptr };
	};

	// Engine grass-manager tunables (INI equivalents). Defaults match vanilla Skyrim SE.
	struct PlacementSettings
	{
		PlacementMode mode{ PlacementMode::Vanilla };
		SmoothPlacementSettings smooth;
		std::uint32_t maxGrassTypesPerTexture{ 2 };
		std::uint32_t grassInstanceStrideWords{ 16 };
		std::uint32_t grassEvalSize{ 2 };
		std::uint32_t minGrassSize{ 20 };
		std::uint32_t grassPatchSize{ grassEvalSize << 7 };
		float alphaThreshold{ 0.0f };
		std::optional<float> waterHeight;
	};

	struct PlacementCounters
	{
		std::uint64_t samplesVisited{ 0 };
		std::uint64_t samplesWithGrassParams{ 0 };
		std::uint64_t grassParamsBuilt{ 0 };
		std::uint64_t latticeCandidates{ 0 };
		std::uint64_t densityRejected{ 0 };
		std::uint64_t waterRejected{ 0 };
		std::uint64_t slopeRejected{ 0 };
		std::uint64_t bladesPlaced{ 0 };
	};

	// One output group (one GRAS) of a cell, in first-use order.
	struct CellGrassGroup
	{
		const GameData::GrassInfo* grass{ nullptr };
		std::string modelPath;
	};

	// A blade exactly as vanilla placement emits it. Rejection only removes blades; it never
	// changes the words of the blades that survive.
	struct BladeCandidate
	{
		std::array<std::uint16_t, 16> words{};
		float position[3]{};
		std::uint32_t groupIndex{ 0 };
	};

	struct CellCandidates
	{
		std::int32_t cellX{ 0 };
		std::int32_t cellY{ 0 };
		std::vector<CellGrassGroup> groups;
		std::vector<BladeCandidate> blades;
		PlacementCounters counters;
	};

	// Per-LAND grass weight grids for smooth placement: for each grass type, the summed weight of
	// the textures that carry it at every vertex, with shared quadrant-edge vertices averaged. Built
	// once in parallel, then read without synchronisation by every worker.
	class SmoothWeightField
	{
	public:
		struct Grid
		{
			const GameData::GrassInfo* grass{ nullptr };
			// Weight * 255 at each of the LAND's 33x33 vertices.
			std::array<std::uint8_t, GameData::LandInfo::VertexCount> weights{};
		};

		// Builds grids for every LAND of the worldspace (the first LAND of a cell wins) and, with
		// matchVanillaDensity, the per-type density scales. Both depend only on the worldspace, so a
		// cell comes out the same however many cells a run places.
		SmoothWeightField(const GameData::StaticWorldSnapshot& a_snapshot, std::span<const GameData::LandInfo> a_worldLands,
			const PlacementSettings& a_settings);

		// The grids of the LAND at a cell, or null when the worldspace has none there.
		[[nodiscard]] const std::vector<Grid>* Find(std::int32_t a_cellX, std::int32_t a_cellY) const;
		// The density multiplier of a grass type (1 unless matchVanillaDensity).
		[[nodiscard]] float DensityScale(GameData::FormID a_grass) const;
		[[nodiscard]] std::size_t CellCount() const { return _grids.size(); }
		[[nodiscard]] std::size_t GridCount() const { return _gridCount; }
		// Vanilla's and smooth's expected world-wide blade count per grass type (before water and
		// slope filters), as used for the scales; empty unless matchVanillaDensity.
		struct ExpectedBlades
		{
			double vanilla{ 0.0 };
			double smooth{ 0.0 };
		};
		[[nodiscard]] const std::unordered_map<GameData::FormID, ExpectedBlades, GameData::FormIDHash>& Expected() const { return _expected; }

	private:
		std::unordered_map<std::uint64_t, std::vector<Grid>> _grids;
		std::unordered_map<GameData::FormID, float, GameData::FormIDHash> _densityScale;
		std::unordered_map<GameData::FormID, ExpectedBlades, GameData::FormIDHash> _expected;
		std::size_t _gridCount{ 0 };
	};

	// One LAND's smooth-placement grids, sorted by grass form ID.
	[[nodiscard]] std::vector<SmoothWeightField::Grid> BuildSmoothWeightGrids(
		const GameData::StaticWorldSnapshot& a_snapshot,
		const GameData::LandInfo& a_land,
		const PlacementSettings& a_settings);

	// Runs the engine's grass placement for one exterior LAND, accepting every blade that passes
	// the density, water and slope tests, in the engine's RNG order.
	[[nodiscard]] CellCandidates GenerateCellCandidates(
		const GameData::StaticWorldSnapshot& a_snapshot,
		const GameData::LandInfo& a_land,
		const PlacementSettings& a_settings);

	// Builds the .cgid payload from the blades that survive. a_rejected is one bit per blade
	// (bit i of word i / 32); an empty span keeps every blade.
	[[nodiscard]] NgioCellCache FinalizeCell(
		const CellCandidates& a_candidates,
		std::span<const std::uint32_t> a_rejected,
		std::uint32_t a_strideWords);

	[[nodiscard]] std::string MakeNgioCacheFileName(std::string_view a_worldEditorID, std::int32_t a_cellX, std::int32_t a_cellY);
	[[nodiscard]] std::string ResolveWorldEditorID(const GameData::StaticWorldSnapshot& a_snapshot, GameData::FormID a_worldFormID);
	[[nodiscard]] bool ExistingNgioCacheLooksValid(const std::filesystem::path& a_path);
}
