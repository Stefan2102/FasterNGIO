#pragma once

#include "Collision/CollisionModel.h"

#include <cstdint>
#include <span>

namespace FasterNGIO::Collision
{
	// NGIO's default Ray-cast-collision-layers: kStatic, kAnimStatic, kTerrain, kDebrisLarge,
	// kStairHelper. Terrain never occurs in a model NIF.
	inline constexpr std::uint32_t kDefaultLayerMask = (1u << 1) | (1u << 2) | (1u << 13) | (1u << 20) | (1u << 31);

	struct ExtractionOptions
	{
		std::uint32_t layerMask{ kDefaultLayerMask };
		// Records a human-readable description of every collision object and shape in
		// CollisionModel::stats.trace.
		bool trace{ false };
		// Also computes CollisionModel::renderAabbMin/Max from the NIF's render shapes.
		bool renderBounds{ false };
	};

	// Extracts the Havok collision of one NIF into model space. Thread-safe.
	[[nodiscard]] CollisionModel ExtractCollision(std::span<const std::uint8_t> a_nifBytes, const ExtractionOptions& a_options);
}
