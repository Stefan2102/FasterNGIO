#pragma once

#include "GameData/StaticWorld.h"

#include <bitset>
#include <cstdint>
#include <unordered_map>
#include <unordered_set>

namespace FasterNGIO::Grass
{
	// Where a worldspace's terrain shows one of a set of land textures, for NGIO's
	// Ray-cast-texture-forms. A point's texture is the one with the highest weight (alpha opacity,
	// or what the alpha layers leave of the base texture) at the nearest vertex of its quadrant, as
	// the game's TES::GetLandTexture is understood to pick it. Built up front, immutable afterwards.
	class LandTextureMask
	{
	public:
		LandTextureMask(const GameData::StaticWorldSnapshot& a_snapshot, GameData::FormID a_worldFormID,
			const std::unordered_set<GameData::FormID, GameData::FormIDHash>& a_textures);

		// Whether the texture at (a_x, a_y) is one of the set; false where the worldspace has no LAND.
		[[nodiscard]] bool Contains(float a_x, float a_y) const;

	private:
		// Per quadrant, per quadrant vertex: whether the vertex's texture is listed.
		using LandBits = std::bitset<GameData::LandInfo::QuadrantCount * GameData::LandInfo::QuadrantVertexCount>;

		std::unordered_map<std::uint64_t, LandBits> _lands;
	};
}
