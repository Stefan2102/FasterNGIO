#include "GameData/StaticWorld.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <unordered_set>

namespace FasterNGIO::GameData
{
	namespace
	{
		ObjectBounds Bounds(const std::optional<tesworld::ObjectBounds>& a_bounds)
		{
			ObjectBounds result;
			if (a_bounds)
			{
				std::copy_n(a_bounds->min, 3, result.min.begin());
				std::copy_n(a_bounds->max, 3, result.max.begin());
				result.present = true;
			}
			return result;
		}

		// TESWorld retains decoded XCLW; grass uses TESObjectCELL::Load's stored, rounded value.
		std::optional<float> EngineCellWaterHeight(std::optional<float> a_value)
		{
			if (!a_value || *a_value >= 2147483648.0f)
				return std::nullopt;
			const auto value = *a_value;
			const auto whole = (std::isnan(value) || value < -2147483648.0f) ? (std::numeric_limits<std::int32_t>::min)() : static_cast<std::int32_t>(value);
			return static_cast<float>(value - static_cast<float>(whole) >= 0.5f ? whole + 1 : whole);
		}
	}

	const WorldInfo* LandDataWorld(const std::unordered_map<FormID, WorldInfo, FormIDHash>& a_worlds, FormID a_world)
	{
		const WorldInfo* result = nullptr;
		std::unordered_set<FormID, FormIDHash> seen;
		while (seen.insert(a_world).second)
		{
			const auto it = a_worlds.find(a_world);
			if (it == a_worlds.end())
				break;
			result = &it->second;
			if (!result->UsesParentLandData())
				break;
			a_world = *result->parentWorldFormID;
		}
		return result;
	}

	StaticWorldSnapshot BuildStaticWorldSnapshot(const std::vector<StaticPluginShard>& a_shards)
	{
		// All plugin decoding, suppression, winning LAND slots and reference bucketing are shared.
		// Project once into the grass engine's fields; keep its sampler and water rules here.
		auto world = tesworld::BuildStaticWorldSnapshot(a_shards, {.enginePartialForms = true});
		StaticWorldSnapshot result;
		result.creationOrder = std::move(world.creationOrder);
		result.firstEditorIDs = std::move(world.firstEditorIDs);
		for (auto& [id, r] : world.baseObjectsByFormID)
		{
			BaseObjectInfo base{.formID = id,
								.signature = r.signature,
								.editorID = std::move(r.editorID),
								.modelPath = std::move(r.modelPath),
								.bounds = Bounds(r.bounds),
								.alternateTextureSets = {},
				.materialObject = r.material};
			for (const auto& alt : r.alternateTextures)
				base.alternateTextureSets.push_back(alt.textureSetFormID);
			result.baseObjectsByFormID.emplace(id, std::move(base));
		}
		for (auto& [id, r] : world.worldsByFormID)
			result.worldsByFormID.emplace(id, WorldInfo{.formID = id,
														.editorID = std::move(r.editorID),
														.parentWorldFormID = r.parentWorld,
														.parentUseFlags = r.parentUseFlags,
														.defaultLandHeight = r.defaultLandHeight.value_or(-2048.0f),
														.defaultWaterHeight = r.defaultWaterHeight.value_or(0.0f)});
		for (auto& [id, r] : world.cellsByFormID)
			result.cellsByFormID.emplace(id, CellInfo{.formID = id,
													  .worldFormID = r.worldFormID,
													  .cellFlags = r.cellFlags,
													  .gridX = r.gridX,
													  .gridY = r.gridY,
													  .waterHeight = EngineCellWaterHeight(r.waterHeight)});
		for (auto& [id, r] : world.grassesByFormID)
			result.grassesByFormID.emplace(id, GrassInfo{.formID = id,
														 .modelPath = std::move(r.modelPath),
														 .density = r.density,
														 .minSlopeDegrees = r.minSlopeDegrees,
														 .maxSlopeDegrees = r.maxSlopeDegrees,
														 .distanceFromWaterLevel = r.distanceFromWaterLevel,
														 .underwaterState = static_cast<GrassWaterState>(r.underwaterState),
														 .positionRange = r.positionRange,
														 .heightRange = r.heightRange,
														 .colorRange = r.colorRange,
														 .wavePeriod = r.wavePeriod,
														 .grassFlags = r.grassFlags,
														 .bounds = Bounds(r.bounds)});
		for (auto& [id, r] : world.landTexturesByFormID)
			result.landTexturesByFormID.emplace(
				id,
				LandTextureInfo{.formID = id, .editorID = std::move(r.editorID), .materialType = r.materialFormID, .grassFormIDs = std::move(r.grassFormIDs)});
		for (auto& [id, r] : world.materialTypesByFormID)
			result.materialTypesByFormID.emplace(id, MaterialTypeInfo{.formID = id, .editorID = std::move(r.editorID), .name = std::move(r.name)});
		for (auto& [id, r] : world.textureSetsByFormID)
			result.textureSetsByFormID.emplace(id, TextureSetInfo{.formID = id, .editorID = std::move(r.editorID), .diffuse = std::move(r.texturePaths[0])});
		for (auto& [id, r] : world.materialObjectsByFormID)
			result.materialObjectsByFormID.emplace(id, MaterialObjectInfo{.formID = id, .editorID = std::move(r)});
		for (auto& [id, records] : world.landsByWorldspace)
		{
			auto& lands = result.landsByWorldspace[id];
			lands.reserve(records.size());
			for (auto& r : records)
			{
				LandInfo land{.formID = r.formID,
							  .parentCell = r.parentCell,
							  .worldFormID = r.worldFormID,
							  .cellX = r.cellX,
							  .cellY = r.cellY,
							  .heights = r.heights,
							  .vertexColors = r.vertexColors,
							  .hasHeights = r.hasHeights,
							  .hasVertexColors = r.hasVertexColors,
				.baseTextures = {}, .alphaTextures = {}, .vertexAlphas = {}};
				if (!land.hasHeights)
				{
					const auto* source = LandDataWorld(result.worldsByFormID, id);
					land.heights.fill(source ? source->defaultLandHeight : -2048.0f);
					land.hasHeights = true;
				}
				for (std::size_t i = 0; i < r.baseTextures.size(); ++i)
					if (r.baseTextures[i])
						land.baseTextures[i] = LandBaseTexture{r.baseTextures[i]->landTextureFormID, r.baseTextures[i]->quadrant};
				for (const auto& a : r.alphaTextures)
					land.alphaTextures.push_back({a.landTextureFormID, a.quadrant, a.layerIndex});
				for (const auto& a : r.vertexAlphas)
					land.vertexAlphas.push_back({a.quadrant, a.layerIndex, a.position, a.opacity});
				lands.push_back(std::move(land));
			}
			// Release each shared bucket as it is projected, rather than retaining another complete world.
			std::vector<tesworld::LandInfo>{}.swap(records);
		}
		for (auto& [key, records] : world.exteriorPlacementsByCell)
		{
			auto& placements = result.exteriorPlacementsByCell[{key.worldFormID, key.x, key.y}];
			placements.reserve(records.size());
			for (const auto& r : records)
			{
				PlacementInfo placed{.formID = r.formID,
									 .parentCell = r.parentCell,
									 .worldFormID = r.worldFormID,
									 .baseFormID = r.baseFormID,
									 .flags = r.flags,
									 .scale = r.scale};
				std::copy_n(r.position, 3, placed.position);
				std::copy_n(r.rotation, 3, placed.rotation);
				placements.push_back(placed);
			}
		}
		return result;
	}
}
