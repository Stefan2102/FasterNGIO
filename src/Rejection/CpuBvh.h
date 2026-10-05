#pragma once

#include "Rejection/NgioRules.h"
#include "Rejection/RejectionConfig.h"
#include "Rejection/WorldIndex.h"

#include <cstdint>
#include <span>
#include <vector>

namespace FasterNGIO::Grass
{
	struct CellCandidates;
}

namespace FasterNGIO::Rejection
{
	// Axis-aligned BVH node. Interior nodes (count == 0) keep their two children adjacent at
	// first and first + 1; leaves cover order[first, first + count).
	struct BvhNode
	{
		float min[3];
		std::uint32_t first;
		float max[3];
		std::uint32_t count;
	};
	static_assert(sizeof(BvhNode) == 32);

	struct CpuBvhStats
	{
		std::uint64_t models{ 0 };
		std::uint64_t primitives{ 0 };
		std::uint64_t modelNodes{ 0 };
		std::uint64_t instances{ 0 };
		std::uint64_t instanceNodes{ 0 };
		double buildSeconds{ 0.0 };
	};

	// The CPU fallback for GPU rejection: the same two-level structure as the DXR path (one BVH
	// per collision model, shared by all its instances, under one BVH of the world's instances),
	// traversed per blade with the exact primitive tests the GPU's intersection shaders use.
	// Immutable once built, so any number of threads can query it without synchronisation.
	class CpuBvh
	{
	public:
		explicit CpuBvh(const WorldIndex& a_world);

		// True when the world-space capsule [a_p, a_q] swept by a_radius overlaps any collision.
		[[nodiscard]] bool CapsuleHitsWorld(const Float3& a_p, const Float3& a_q, float a_radius) const;

		// What the world-space capsule touches, by instance role; stops once nothing can change the
		// blade's fate (an ordinary hit, and a cliff hit when the world has cliffs).
		[[nodiscard]] VolumeHits ClassifyCapsule(const Float3& a_p, const Float3& a_q, float a_radius) const;

		// Appends every surface segment [a_p, a_q] meets, in no particular order (see
		// GrassQueryMath.hlsli's SegmentHit).
		void SegmentHitsWorld(const Float3& a_p, const Float3& a_q, std::vector<WorldSegmentHit>& a_hits) const;

		// One reject bit per blade, as RejectCellOnCpu returns.
		[[nodiscard]] std::vector<std::uint32_t> RejectCell(const Grass::CellCandidates& a_cell, std::span<const QueryShape> a_shapes) const;

		[[nodiscard]] const CpuBvhStats& Stats() const { return _stats; }

	private:
		struct ModelBvh
		{
			std::vector<BvhNode> nodes;
			// Primitive references in leaf order: kind in the top two bits, index below.
			std::vector<std::uint32_t> primitives;
		};

		[[nodiscard]] bool CapsuleHitsInstance(const Instance& a_instance, const Float3& a_p, const Float3& a_q, float a_radius) const;

		const WorldIndex& _world;
		std::vector<ModelBvh> _models;
		std::vector<BvhNode> _instanceNodes;
		std::vector<std::uint32_t> _instanceOrder;
		CpuBvhStats _stats;
	};
}
