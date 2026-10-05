#pragma once

#include "Collision/CollisionModel.h"
#include "Rejection/RejectionFeatures.h"
#include "Rejection/WorldIndex.h"

#include <array>
#include <cstdint>
#include <functional>
#include <vector>

namespace FasterNGIO::GameData
{
	struct GrassInfo;
}

namespace FasterNGIO::Rejection
{
	inline constexpr std::uint32_t kNoInstance = 0xFFFFFFFFu;

	// What a blade's query volume touched, by instance role (the GPU's first pass, or the CPU BVH).
	struct VolumeHits
	{
		bool ordinary{ false };
		bool cliff{ false };
		// The first instance with ignored shapes touched, or kNoInstance.
		std::uint32_t partIgnored{ kNoInstance };
	};

	// One vertical ray of the cliff check: the nearest instance it meets and how far up the segment
	// its highest hit is.
	struct CliffRay
	{
		std::uint32_t closest{ kNoInstance };
		float highestT{ 0.0f };
	};

	// The rays NGIO casts for a blade that touched a cliff (RaycastHelper::CreateGrassCliff).
	struct CliffRays
	{
		// Up kCliffRayLength from the blade: the nearest instance, and the highest hit on a cliff
		// instance with its normal facing up (cliffT < 0: none).
		std::uint32_t upClosest{ kNoInstance };
		float cliffT{ -1.0f };
		Collision::Float3 cliffNormal;
		// Beside the cliff point, at +x, -x, +y, -y, each spanning the cliff point's height window;
		// cast only when the up ray found a cliff.
		std::array<CliffRay, 4> neighbours{};
	};

	// Where the neighbour rays of a blade at a_position go once the up ray found a cliff point at
	// height a_cliffZ: [bottom, top] for each of +x, -x, +y, -y.
	struct CliffNeighbourSegments
	{
		std::array<Collision::Float3, 4> bottom;
		std::array<Collision::Float3, 4> top;
	};
	[[nodiscard]] CliffNeighbourSegments MakeCliffNeighbourSegments(const float (&a_position)[3], float a_cliffZ, float a_offset, bool a_steep);

	// How far beside the cliff point the neighbours are, from the grass type's bounds.
	[[nodiscard]] float CliffNeighbourOffset(const GameData::GrassInfo& a_grass);

	// Reduces a ray's hits to the nearest instance and the highest hit (on any instance, or only on
	// cliff instances when a_cliffsOnly).
	struct RayReduction
	{
		std::uint32_t closest{ kNoInstance };
		float closestT{ 2.0f };
		float highestT{ -1.0f };
		Collision::Float3 highestNormal;
	};
	[[nodiscard]] RayReduction ReduceRayHits(const WorldIndex& a_world, const std::vector<WorldSegmentHit>& a_hits, bool a_cliffsOnly);

	// Appends every surface a segment meets (CpuBvh::SegmentHitsWorld, or the brute-force reference).
	using SegmentHitsFunction = std::function<void(const Collision::Float3&, const Collision::Float3&, std::vector<WorldSegmentHit>&)>;

	// The cliff rays for a blade on the CPU, as the GPU's cliff pass computes them.
	[[nodiscard]] CliffRays TraceCliffRays(const SegmentHitsFunction& a_segmentHits, const WorldIndex& a_world, const float (&a_position)[3], float a_neighbourOffset);

	// Whether a blade at a_position is rejected by what its volume touched, cliffs aside: an ordinary
	// object, or an object with ignored shapes away from those shapes (its nearest render shape is
	// not one of them).
	[[nodiscard]] bool VolumeRejects(const VolumeHits& a_hits, const WorldIndex& a_world, const RejectionFeatures& a_features, const float (&a_position)[3]);

	enum class CliffOutcome
	{
		// Not a cliff after all (the ray up met something else first, or the grass grows only under
		// water): the cliff is ignored and the blade judged on its other hits.
		NotCliff,
		// The blade moves onto the cliff.
		Moved,
		// The cliff point failed a check (slope, shapes, neighbours): the blade is rejected.
		Failed
	};

	struct CliffDecision
	{
		CliffOutcome outcome{ CliffOutcome::NotCliff };
		float z{ 0.0f };
		Collision::Float3 normal;
	};

	[[nodiscard]] CliffDecision DecideCliff(const CliffRays& a_rays, const WorldIndex& a_world, const RejectionFeatures& a_features, const GameData::GrassInfo& a_grass,
		const float (&a_position)[3]);

	// Whether a blade's volume hits call for the cliff rays.
	[[nodiscard]] inline bool NeedsCliffRays(const VolumeHits& a_hits, const RejectionFeatures& a_features)
	{
		return a_features.cliffs && a_hits.cliff;
	}

	// The final word on one blade: rejected, kept, or kept and moved onto a cliff.
	struct BladeDecision
	{
		bool rejected{ false };
		bool moved{ false };
		float z{ 0.0f };
		Collision::Float3 normal;
	};

	// a_rays: the blade's cliff rays when NeedsCliffRays, else ignored.
	[[nodiscard]] BladeDecision DecideBlade(const VolumeHits& a_hits, const CliffRays& a_rays, const WorldIndex& a_world, const RejectionFeatures& a_features,
		const GameData::GrassInfo& a_grass, const float (&a_position)[3]);

	// Whether a world needs the per-role path at all: without cliffs or objects with ignored shapes,
	// rejection is the plain "anything in the volume" test.
	[[nodiscard]] inline bool UsesRoles(const WorldIndex& a_world)
	{
		return a_world.Stats().cliffInstances != 0 || a_world.Stats().partIgnoredInstances != 0;
	}
}
