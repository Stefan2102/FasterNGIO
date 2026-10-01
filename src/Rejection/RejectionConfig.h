#pragma once

#include "Collision/NifCollisionExtractor.h"
#include "GameData/GameData.h"

#include <cstdint>
#include <unordered_set>

namespace FasterNGIO::Rejection
{
	// NGIO's Ray-cast-mode.
	enum class QueryMode : std::uint32_t
	{
		Ray = 0,
		Capsule = 1,
		Box = 2
	};

	// NGIO [RayCastConfig] settings, with NGIO's defaults.
	struct RejectionConfig
	{
		bool enabled{ true };
		float rayHeight{ 150.0f };
		float rayDepth{ 5.0f };
		QueryMode mode{ QueryMode::Capsule };
		float rayWidth{ 0.0f };
		float rayWidthMultiplier{ 0.3f };
		std::uint32_t layerMask{ Collision::kDefaultLayerMask };
		std::unordered_set<GameData::FormID, GameData::FormIDHash> ignoreForms;
		std::unordered_set<GameData::FormID, GameData::FormIDHash> ignoreGrassForms;
	};

	// The world-space query volume for one grass type, relative to a blade at (x, y, z).
	// Ray and capsule modes test the segment from (x, y, z - depth) to (x, y, z + height), swept by
	// `radius`. Box mode, like NGIO's box phantom, is an axis-aligned box *centred* on
	// (x, y, z - depth) with half extents (halfExtentX, halfExtentY, (depth + height) / 2).
	struct QueryShape
	{
		bool test{ true };
		float depth{ 0.0f };
		float height{ 0.0f };
		float radius{ 0.0f };
		float halfExtentX{ 0.0f };
		float halfExtentY{ 0.0f };

		// Horizontal reach of the volume around the blade, for culling.
		[[nodiscard]] float Reach() const { return radius > halfExtentX ? (radius > halfExtentY ? radius : halfExtentY) : (halfExtentX > halfExtentY ? halfExtentX : halfExtentY); }
	};

	// Reproduces NGIO's hkpPhantomCast shape sizing for a grass type.
	[[nodiscard]] QueryShape MakeQueryShape(const RejectionConfig& a_config, const GameData::GrassInfo& a_grass);
}
