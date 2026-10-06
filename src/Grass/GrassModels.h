#pragma once

#include "GameData/StaticWorld.h"

#include <cstdint>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace FasterNGIO::Archives
{
	class ArchiveResolver;
}

namespace FasterNGIO::Grass
{
	// How many blades each grass type's cache blocks hold: the engine's instances per group for the
	// GRAS model (BladesPerBlock), measured once per run from the models the load order resolves.
	struct GrassModelLayout
	{
		std::unordered_map<GameData::FormID, std::uint32_t, GameData::FormIDHash> bladesPerBlock;
		std::size_t models{ 0 };
		// Models no loose file or archive provides, so the game cannot load them, and the grass types
		// using them (PlacementSettings::unloadableGrass: placed nowhere).
		std::vector<std::string> missing;
		std::unordered_set<GameData::FormID, GameData::FormIDHash> missingGrass;
		// Models whose root's first child is not a BSTriShape; their grass falls back to
		// kMaxBladesPerBlock.
		std::vector<std::string> unsupported;
	};

	[[nodiscard]] GrassModelLayout MeasureGrassModels(const GameData::StaticWorldSnapshot& a_snapshot, const Archives::ArchiveResolver& a_resolver);
}
