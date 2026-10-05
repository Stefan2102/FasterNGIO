#pragma once

#include "GameData/LoadOrder.h"
#include "GameData/Records.h"

#include <cstddef>
#include <span>
#include <unordered_map>
#include <vector>

namespace FasterNGIO::GameData
{
	// A deleted or ignored record: it hides every earlier plugin's version of the form.
	struct StaticRecordSuppressor
	{
		FormID formID{};
		FourCC signature{ 0 };
	};

	// What one plugin contributes, in file order, before overrides are resolved.
	struct StaticPluginShard
	{
		LoadOrderEntry entry;
		std::vector<BaseObjectInfo> baseObjects;
		std::vector<LandTextureInfo> landTextures;
		std::vector<GrassInfo> grasses;
		std::vector<WorldInfo> worlds;
		std::vector<CellInfo> cells;
		std::vector<LandInfo> lands;
		std::vector<PlacementInfo> placements;
		std::vector<StaticRecordSuppressor> suppressors;
	};

	// Parses the records FasterNGIO needs from one plugin of a prepared load order. Safe to call for
	// several plugins at once.
	[[nodiscard]] StaticPluginShard ParseStaticWorldShard(const LoadOrderEntry& a_entry, std::span<const LoadOrderEntry> a_loadOrder);

	// The winning version of every record, with LAND and placed references bucketed by exterior
	// cell. Immutable once built.
	struct StaticWorldSnapshot
	{
		std::unordered_map<FormID, BaseObjectInfo, FormIDHash> baseObjectsByFormID;
		std::unordered_map<FormID, LandTextureInfo, FormIDHash> landTexturesByFormID;
		std::unordered_map<FormID, GrassInfo, FormIDHash> grassesByFormID;
		std::unordered_map<FormID, WorldInfo, FormIDHash> worldsByFormID;
		std::unordered_map<FormID, CellInfo, FormIDHash> cellsByFormID;
		// LAND whose cell is an exterior cell with grid coordinates.
		std::unordered_map<FormID, std::vector<LandInfo>, FormIDHash> landsByWorldspace;
		// Enabled references whose base object is known, by exterior cell.
		std::unordered_map<CellKey, std::vector<PlacementInfo>, CellKeyHash> exteriorPlacementsByCell;
	};

	// Resolves the shards (in load order) the way the engine does: the last plugin's version of a
	// form wins, and a deleted or ignored version removes it.
	[[nodiscard]] StaticWorldSnapshot BuildStaticWorldSnapshot(std::span<const StaticPluginShard> a_shards);
}
