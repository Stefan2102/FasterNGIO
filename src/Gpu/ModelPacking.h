#pragma once

#include "Collision/CollisionModel.h"
#include "Rejection/Bounds.h"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace FasterNGIO::Gpu
{
	// One collision model laid out as GrassRejection.hlsl reads it (shaders/Shared/RejectionLayout.hlsli),
	// followed by the AABBs of each BLAS geometry.
	struct PackedModel
	{
		std::vector<std::byte> bytes;
		// One BLAS geometry per primitive kind present: its AABBs' offset in bytes and their count.
		struct Geometry
		{
			Rejection::PrimitiveKind kind;
			std::uint64_t aabbOffset;
			std::uint32_t count;
		};
		std::vector<Geometry> geometries;
		// Bit k set when geometry kind k is present (the hit group subset the model uses).
		std::uint32_t kindMask{ 0 };
	};

	// Each primitive's AABB is grown by its own radius plus a_inflation (the widest query radius in
	// the model's space), so it holds every query capsule that could touch the primitive.
	[[nodiscard]] PackedModel PackModel(const Collision::CollisionModel& a_model, float a_inflation);
}
