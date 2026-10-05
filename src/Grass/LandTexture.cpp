#include "Grass/LandTexture.h"

#include "Grass/Internal/PlacementCommon.h"

#include <oneapi/tbb/parallel_for.h>

#include <algorithm>
#include <cmath>
#include <vector>

namespace FasterNGIO::Grass
{
	LandTextureMask::LandTextureMask(const GameData::StaticWorldSnapshot& a_snapshot, GameData::FormID a_worldFormID,
		const std::unordered_set<GameData::FormID, GameData::FormIDHash>& a_textures)
	{
		const auto landsIt = a_snapshot.landsByWorldspace.find(a_worldFormID);
		if (landsIt == a_snapshot.landsByWorldspace.end() || a_textures.empty()) {
			return;
		}
		// One LAND per cell, the first seen (as SelectLands).
		std::vector<const GameData::LandInfo*> lands;
		std::unordered_set<std::uint64_t> seen;
		for (const auto& land : landsIt->second) {
			if (land.cellX && land.cellY && seen.insert(GameData::PackCellCoords(*land.cellX, *land.cellY)).second) {
				lands.push_back(std::addressof(land));
			}
		}
		std::vector<LandBits> bits(lands.size());
		oneapi::tbb::parallel_for(std::size_t{ 0 }, lands.size(), [&](std::size_t i) {
			const auto weights = Internal::BuildVanillaQuadrantWeights(*lands[i]);
			for (std::size_t quadrant = 0; quadrant < GameData::LandInfo::QuadrantCount; ++quadrant) {
				for (std::size_t sample = 0; sample < GameData::LandInfo::QuadrantVertexCount; ++sample) {
					const Internal::TextureSampleWeight* top = nullptr;
					for (const auto& weight : weights[quadrant][sample]) {
						// The base texture is listed last, so a tie keeps the alpha layer.
						if (!top || weight.weight > top->weight) {
							top = std::addressof(weight);
						}
					}
					if (top && a_textures.contains(top->formID)) {
						bits[i].set(quadrant * GameData::LandInfo::QuadrantVertexCount + sample);
					}
				}
			}
		});
		for (std::size_t i = 0; i < lands.size(); ++i) {
			if (bits[i].any()) {
				_lands.emplace(GameData::PackCellCoords(*lands[i]->cellX, *lands[i]->cellY), bits[i]);
			}
		}
	}

	bool LandTextureMask::Contains(float a_x, float a_y) const
	{
		constexpr float kCellSize = GameData::kSkyrimTerrainCellSize;
		constexpr auto kQuadrantQuads = static_cast<int>(GameData::LandInfo::QuadrantVertexSide - 1);
		const auto cellX = static_cast<std::int32_t>(std::floor(a_x / kCellSize));
		const auto cellY = static_cast<std::int32_t>(std::floor(a_y / kCellSize));
		const auto it = _lands.find(GameData::PackCellCoords(cellX, cellY));
		if (it == _lands.end()) {
			return false;
		}
		const auto localX = a_x - static_cast<float>(cellX) * kCellSize;
		const auto localY = a_y - static_cast<float>(cellY) * kCellSize;
		const int quadrantX = localX >= kCellSize * 0.5f ? 1 : 0;
		const int quadrantY = localY >= kCellSize * 0.5f ? 1 : 0;
		const auto vertex = [&](float a_local, int a_quadrant) {
			return std::clamp(static_cast<int>(std::lround(a_local / Internal::kVertexSpacing)) - a_quadrant * kQuadrantQuads, 0, kQuadrantQuads);
		};
		const auto quadrant = static_cast<std::size_t>(quadrantY * 2 + quadrantX);
		const auto sample = static_cast<std::size_t>(vertex(localY, quadrantY) * (kQuadrantQuads + 1) + vertex(localX, quadrantX));
		return it->second.test(quadrant * GameData::LandInfo::QuadrantVertexCount + sample);
	}
}
