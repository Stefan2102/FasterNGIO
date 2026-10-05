#pragma once

#include "Collision/CollisionModel.h"
#include "GameData/FormID.h"

#include <cstdint>

namespace FasterNGIO::GameData
{
	struct GrassInfo;
}

namespace FasterNGIO::Rejection
{
	// NGIO's Ray-cast-mode. NGIO's box mode (2) is not implemented.
	enum class QueryMode : std::uint32_t
	{
		Ray = 0,
		Capsule = 1
	};

	// NGIO [RayCastConfig] settings, with NGIO's defaults.
	struct RejectionConfig
	{
		float rayHeight{ 150.0f };
		float rayDepth{ 5.0f };
		QueryMode mode{ QueryMode::Capsule };
		float rayWidth{ 0.0f };
		float rayWidthMultiplier{ 0.3f };
	};

	// The query volume for one grass type, relative to a blade at (x, y, z): the segment from
	// (x, y, z - depth) to (x, y, z + height), swept by radius (0 in ray mode). The radius is also
	// the volume's horizontal reach around the blade.
	struct QueryShape
	{
		float depth{ 0.0f };
		float height{ 0.0f };
		float radius{ 0.0f };
	};

	// Reproduces NGIO's hkpPhantomCast shape sizing for a grass type.
	[[nodiscard]] QueryShape MakeQueryShape(const RejectionConfig& a_config, const GameData::GrassInfo& a_grass);

	// The segment a blade's query sweeps, in world space.
	struct QuerySegment
	{
		Collision::Float3 bottom;
		Collision::Float3 top;
	};

	[[nodiscard]] inline QuerySegment BladeSegment(const QueryShape& a_shape, const float (&a_position)[3])
	{
		return QuerySegment{
			.bottom = { a_position[0], a_position[1], a_position[2] - a_shape.depth },
			.top = { a_position[0], a_position[1], a_position[2] + a_shape.height },
		};
	}
}
