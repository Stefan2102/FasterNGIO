#pragma once

#include "Grass/Placement.h"

#include <array>
#include <cstdint>
#include <functional>
#include <optional>
#include <vector>

// What vanilla and smooth placement share: the engine's terrain sampling, filters and blade
// encoding, and the texture-weight bookkeeping both start from.
namespace FasterNGIO::Grass::Internal
{
	using GameData::LandInfo;

	// Grass caches are written per 12x12-cell block; blade positions are stored as half floats
	// relative to the block's corner.
	inline constexpr std::int32_t kBlockCells = 12;
	// Distance between LAND vertices (32 quads per cell).
	inline constexpr float kVertexSpacing = GameData::kSkyrimTerrainCellSize / 32.0f;
	// GRAS density is a percentage.
	inline constexpr float kDensityPercent = 0.01f;

	struct TerrainSample
	{
		float height{ 0.0f };
		float normal[3]{ 0.0f, 0.0f, 1.0f };
		float color[3]{ 1.0f, 1.0f, 1.0f };
	};

	// One grass type the engine places around a quadrant vertex: its density at the 3x3
	// neighbouring vertices.
	struct GrassParamBuild
	{
		const GameData::GrassInfo* grass{ nullptr };
		std::array<float, 9> density{};
	};

	struct TextureSampleWeight
	{
		GameData::FormID formID{};
		float weight{ 0.0f };
		bool isBaseTexture{ false };
		std::uint16_t layerIndex{ 0 };
	};

	// Each quadrant's texture weights at each of its 17x17 vertices.
	using QuadrantSampleWeights = std::array<std::array<std::vector<TextureSampleWeight>, LandInfo::QuadrantVertexCount>, LandInfo::QuadrantCount>;

	[[nodiscard]] std::uint16_t FloatToHalfBits(float a_value);
	[[nodiscard]] float HalfBitsToFloat(std::uint16_t a_value);

	[[nodiscard]] inline float Clamp01(float a_value)
	{
		return (std::min)((std::max)(a_value, 0.0f), 1.0f);
	}

	[[nodiscard]] inline float Lerp(float a_lhs, float a_rhs, float a_t)
	{
		return a_lhs + (a_rhs - a_lhs) * a_t;
	}

	// Bilinear interpolation between corners v00 (fx = fy = 0), v10, v01 and v11.
	[[nodiscard]] inline float Bilerp(float a_v00, float a_v10, float a_v01, float a_v11, float a_fx, float a_fy)
	{
		return Lerp(Lerp(a_v00, a_v10, a_fx), Lerp(a_v01, a_v11, a_fx), a_fy);
	}

	// The corner of the cache block a cell belongs to, in game units (one axis).
	[[nodiscard]] inline float BlockBase(std::int32_t a_cell)
	{
		return static_cast<float>((a_cell / kBlockCells) * kBlockCells) * GameData::kSkyrimTerrainCellSize;
	}

	// a_coordinate as the cache will store it: a half float relative to the block corner.
	[[nodiscard]] inline float QuantizeToBlock(float a_coordinate, float a_blockBase)
	{
		return a_blockBase + HalfBitsToFloat(FloatToHalfBits(a_coordinate - a_blockBase));
	}

	// The LAND's cell water height, else the settings' fallback.
	[[nodiscard]] std::optional<float> CellWaterHeight(const GameData::StaticWorldSnapshot& a_snapshot, const LandInfo& a_land, const PlacementSettings& a_settings);

	// Lattice points per side of one placement patch for a grass type: the patch diameter divided by
	// iMinGrassSize or by the GRAS position range, whichever gives fewer. Zero places nothing.
	[[nodiscard]] std::uint32_t PatchLatticeSide(const GameData::GrassInfo& a_grass, const PlacementSettings& a_settings);

	[[nodiscard]] QuadrantSampleWeights BuildVanillaQuadrantWeights(const LandInfo& a_land);

	// Height, normal and vertex colour at (a_x, a_y), on the engine's alternating triangle split of
	// each LAND quad.
	[[nodiscard]] TerrainSample SampleTerrain(const LandInfo& a_land, float a_x, float a_y);

	[[nodiscard]] bool PassesWaterFilter(const GameData::GrassInfo& a_grass, float a_height, float a_waterHeight);
	[[nodiscard]] bool PassesSlopeFilter(const GameData::GrassInfo& a_grass, const TerrainSample& a_sample);

	// The grass types the engine takes from a land texture, in GNAM order: only GNAMs that resolve
	// to a GRAS count, and the list ends once the count exceeds iMaxGrassTypesPerTexure (the
	// engine tests `count > max` before taking each one), so the default of 2 yields 3 types.
	template <class Visit>
	void ForEachTextureGrass(const GameData::StaticWorldSnapshot& a_snapshot, const GameData::LandTextureInfo& a_texture, std::uint32_t a_maxTypes,
		Visit&& a_visit)
	{
		std::uint32_t used = 0;
		for (const auto grassFormID : a_texture.grassFormIDs) {
			const auto grassIt = a_snapshot.grassesByFormID.find(grassFormID);
			if (grassIt == a_snapshot.grassesByFormID.end()) {
				continue;
			}
			if (used > a_maxTypes) {
				break;
			}
			++used;
			if (!grassIt->second.modelPath.empty()) {
				a_visit(grassIt->second);
			}
		}
	}

	// The grass types (and their 3x3 densities) the engine places around one quadrant vertex.
	[[nodiscard]] std::vector<GrassParamBuild> BuildGrassParamsForSample(const GameData::StaticWorldSnapshot& a_snapshot, const QuadrantSampleWeights& a_weights,
		std::uint8_t a_quadrant, std::size_t a_sample, const PlacementSettings& a_settings);

	// Writes a blade's 16 words: its position relative to the cache block, height, brightness, the
	// orientation basis (tilted to the terrain normal for fit-to-slope grass) and its height offset.
	void EncodeBlade(BladeCandidate& a_blade, std::int32_t a_cellX, std::int32_t a_cellY, float a_x, float a_y, const TerrainSample& a_sample,
		const GameData::GrassInfo& a_grass, float a_brightness, float a_orientation, float a_heightRandom);

	// What follows once a blade's position is accepted, in both placements: snap it to the cache's
	// half-float grid, sample the terrain, apply the water and slope filters, then draw colour,
	// orientation and height (in that order) from a_signedRandom, a value in [-1, 1), and append the
	// blade.
	template <class SignedRandom>
	void EmitBlade(CellCandidates& a_cell, std::uint32_t a_groupIndex, const LandInfo& a_land, const GameData::GrassInfo& a_grass, float a_x, float a_y,
		std::optional<float> a_waterHeight, SignedRandom&& a_signedRandom)
	{
		const auto x = QuantizeToBlock(a_x, BlockBase(a_cell.cellX));
		const auto y = QuantizeToBlock(a_y, BlockBase(a_cell.cellY));
		const auto terrain = SampleTerrain(a_land, x, y);
		if (a_waterHeight && !PassesWaterFilter(a_grass, terrain.height, *a_waterHeight)) {
			return;
		}
		if (!PassesSlopeFilter(a_grass, terrain)) {
			return;
		}
		// Rec. 601 luma of the terrain's vertex colour, varied by the GRAS colour range.
		const auto colorRandom = a_signedRandom() * a_grass.colorRange;
		const auto luma = terrain.color[0] * 0.299f + terrain.color[1] * 0.587f + terrain.color[2] * 0.114f;
		const auto brightness = Clamp01(luma * (1.0f - a_grass.colorRange + colorRandom));
		const auto orientation = a_signedRandom();
		const auto heightRandom = a_signedRandom();
		auto& blade = a_cell.blades.emplace_back();
		blade.groupIndex = a_groupIndex;
		EncodeBlade(blade, a_cell.cellX, a_cell.cellY, x, y, terrain, a_grass, brightness, orientation, heightRandom);
	}

	// The two placements behind GenerateCellCandidates; a_land has a cell and heights.
	[[nodiscard]] CellCandidates GenerateVanillaCellCandidates(const GameData::StaticWorldSnapshot& a_snapshot, const LandInfo& a_land, const PlacementSettings& a_settings);
	[[nodiscard]] CellCandidates GenerateSmoothCellCandidates(const GameData::StaticWorldSnapshot& a_snapshot, const LandInfo& a_land, const PlacementSettings& a_settings);

	// Vanilla's expected blades per grass type for one LAND (the exact sum of its lattice's
	// acceptance probabilities), passed to a_add in the order vanilla visits them.
	void AddExpectedVanillaBlades(const GameData::StaticWorldSnapshot& a_snapshot, const LandInfo& a_land, const PlacementSettings& a_settings,
		const std::function<void(GameData::FormID, double)>& a_add);
}
