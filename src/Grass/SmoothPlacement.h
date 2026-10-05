#pragma once

#include "Grass/Placement.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <unordered_map>
#include <vector>

namespace FasterNGIO::Grass
{
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

		// Vanilla's and smooth's expected blade count for one grass type, before the water and slope
		// filters (which both apply alike).
		struct ExpectedBlades
		{
			double vanilla{ 0.0 };
			double smooth{ 0.0 };
		};

		// Builds grids for every LAND of the worldspace (the first LAND of a cell wins) and, with
		// matchVanillaDensity, the per-type density scales. Both depend only on the worldspace, so a
		// cell comes out the same however many cells a run places.
		SmoothWeightField(const GameData::StaticWorldSnapshot& a_snapshot, std::span<const GameData::LandInfo> a_worldLands, const PlacementSettings& a_settings);

		// The grids of the LAND at a cell, or null when the worldspace has none there.
		[[nodiscard]] const std::vector<Grid>* Find(std::int32_t a_cellX, std::int32_t a_cellY) const;
		// The density multiplier of a grass type (1 unless matchVanillaDensity).
		[[nodiscard]] float DensityScale(GameData::FormID a_grass) const;
		[[nodiscard]] std::size_t CellCount() const { return _grids.size(); }
		[[nodiscard]] std::size_t GridCount() const { return _gridCount; }
		// World-wide expected blades per grass type, as used for the scales; empty unless
		// matchVanillaDensity.
		[[nodiscard]] const std::unordered_map<GameData::FormID, ExpectedBlades, GameData::FormIDHash>& Expected() const { return _expected; }

	private:
		std::unordered_map<std::uint64_t, std::vector<Grid>> _grids;
		std::unordered_map<GameData::FormID, float, GameData::FormIDHash> _densityScale;
		std::unordered_map<GameData::FormID, ExpectedBlades, GameData::FormIDHash> _expected;
		std::size_t _gridCount{ 0 };
	};

	// One LAND's smooth-placement grids, sorted by grass form ID.
	[[nodiscard]] std::vector<SmoothWeightField::Grid> BuildSmoothWeightGrids(const GameData::StaticWorldSnapshot& a_snapshot, const GameData::LandInfo& a_land,
		const PlacementSettings& a_settings);
}
