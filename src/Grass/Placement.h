#pragma once

#include "GameData/StaticWorld.h"

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
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
	// grid, quadrant and cell seams and on/off steps all disappear (see AGENTS.md, "Smooth placement
	// is seam-free and calibrated").
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
		// Cached weights for the cell and its neighbours (see SmoothPlacement.h). Without one, each
		// cell's weights are built on the fly and its border vertices are not merged with the
		// neighbouring cells'.
		const SmoothWeightField* field{ nullptr };
	};

	// Engine grass-manager tunables (INI equivalents). Defaults match vanilla Skyrim SE.
	struct PlacementSettings
	{
		PlacementMode mode{ PlacementMode::Vanilla };
		SmoothPlacementSettings smooth;
		// iMaxGrassTypesPerTexure: the engine takes one more than this per land texture.
		std::uint32_t maxGrassTypesPerTexture{ 2 };
		std::uint32_t grassEvalSize{ 2 };
		// iMinGrassSize: the closest lattice spacing, in game units.
		std::uint32_t minGrassSize{ 20 };
		// Half the side of the patch placed around each evaluated vertex.
		std::uint32_t grassPatchSize{ grassEvalSize << 7 };
		// fTexturePctThreshold.
		float alphaThreshold{ 0.0f };
		// Water height for cells flagged Has Water without an XCLW of their own, in place of the
		// worldspace's default.
		std::optional<float> waterHeight;
		// NGIO's Global-grass-scale: multiplies every blade's final scale.
		float globalScale{ 1.0f };
		// Seasons of Skyrim: a land texture -> the land texture whose grass list it takes this season.
		const std::unordered_map<GameData::FormID, GameData::FormID, GameData::FormIDHash>* landTextureGrass{ nullptr };
		// Grass types whose model the game cannot load (GrassModelLayout::missingGrass). The engine
		// counts them against iMaxGrassTypesPerTexure but places none (no RNG draws), as for a GRAS
		// without a model; a cache group for one would make the game misread the rest of the file.
		const std::unordered_set<GameData::FormID, GameData::FormIDHash>* unloadableGrass{ nullptr };
	};

	// One output group (one GRAS) of a cell, in first-use order.
	struct CellGrassGroup
	{
		const GameData::GrassInfo* grass{ nullptr };
		// What the .cgid stores (GameData::GrassCacheModelPath), not the resource key.
		std::string modelPath;
	};

	// The 16-bit words of one blade in a .cgid, as the engine lays them out.
	inline constexpr std::uint32_t kBladeWords = 16;

	// A placed blade, encoded as the engine stores it. Rejection removes blades, and moves the ones
	// NGIO's grass cliffs lift onto a cliff (re-encoded from the draws kept here).
	struct BladeCandidate
	{
		std::array<std::uint16_t, kBladeWords> words{};
		float position[3]{};
		std::uint32_t groupIndex{ 0 };
		// The quadrant of the cell (0-3; bit 0 east, bit 1 north) the blade is cached under: the one
		// vanilla placed it from, else the one it stands in. The engine flushes each type's blades into
		// blocks once per quadrant (see CellCache.h).
		std::uint8_t quadrant{ 0 };
		// The colour, orientation and height draws the words were encoded from.
		float brightness{ 0.0f };
		float orientation{ 0.0f };
		float heightRandom{ 0.0f };
	};

	struct CellCandidates
	{
		std::int32_t cellX{ 0 };
		std::int32_t cellY{ 0 };
		std::vector<CellGrassGroup> groups;
		std::vector<BladeCandidate> blades;
	};

	// Moves a blade of a_cell onto a surface at height a_z, standing on a_normal (NGIO's grass cliffs,
	// which make the blade fit to the slope): its words are encoded again from its own draws.
	void MoveBlade(BladeCandidate& a_blade, const CellCandidates& a_cell, float a_z, const float (&a_normal)[3], float a_globalScale);

	// Places grass on one exterior LAND, keeping every blade that passes the density, water and
	// slope tests: vanilla in the engine's RNG order, or smooth. Empty for a LAND without a cell or
	// heights.
	[[nodiscard]] CellCandidates GenerateCellCandidates(const GameData::StaticWorldSnapshot& a_snapshot, const GameData::LandInfo& a_land, const PlacementSettings& a_settings);
}
