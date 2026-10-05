#pragma once

#include "Collision/CollisionModel.h"

#include <cstdint>
#include <span>
#include <string_view>

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
		// Also fills CollisionModel::rootName and renderShapes.
		bool renderShapes{ false };
		// Experimental: a model with collision in a kept layer is represented by the triangles of
		// its render shapes instead (CollisionModel::stats.renderGeometry).
		bool renderGeometry{ false };
	};

	// The render shape owning the vertex nearest a_point (model space), as NGIO picks the part of a
	// reference a blade is on; null when the model has no render shapes.
	[[nodiscard]] const RenderShape* NearestRenderShape(const CollisionModel& a_model, const Float3& a_point);

	// Whether a_shape is the one an NGIO object file names: its own name, or "<root name>:<name>".
	[[nodiscard]] bool RenderShapeNamed(const CollisionModel& a_model, const RenderShape& a_shape, std::string_view a_name);

	// Extracts the Havok collision of one NIF into model space. Thread-safe.
	[[nodiscard]] CollisionModel ExtractCollision(std::span<const std::uint8_t> a_nifBytes, const ExtractionOptions& a_options);
}
