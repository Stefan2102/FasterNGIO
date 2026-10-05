#pragma once

#include "Rejection/NgioRules.h"
#include "Rejection/RejectionConfig.h"
#include "Rejection/WorldIndex.h"

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace FasterNGIO::Grass
{
	struct CellCandidates;
}

namespace FasterNGIO::Rejection
{
	// Tests every blade of a cell against the world's collision on the CPU, by brute force over
	// the instances binned to the cell. This is the correctness reference for the GPU path.
	// a_shapes is indexed by the cell's group index. Returns one reject bit per blade.
	[[nodiscard]] std::vector<std::uint32_t> RejectCellOnCpu(
		const WorldIndex& a_world,
		const Grass::CellCandidates& a_cell,
		std::span<const QueryShape> a_shapes);

	// Describes what a capsule query hits on the CPU (instance, model, primitive kind), for
	// diagnosing disagreements with the GPU. Empty when nothing is hit.
	[[nodiscard]] std::string ExplainCapsule(const WorldIndex& a_world, std::int32_t a_cellX, std::int32_t a_cellY, const Float3& a_p, const Float3& a_q, float a_radius);

	// What a capsule query touches, by instance role, by brute force over the instances binned to
	// the cell (CpuBvh::ClassifyCapsule's reference; it does not stop early).
	[[nodiscard]] VolumeHits ClassifyCapsuleBruteForce(const WorldIndex& a_world, std::int32_t a_cellX, std::int32_t a_cellY, const Float3& a_p, const Float3& a_q,
		float a_radius);

	// Appends every surface a vertical segment [a_p, a_q] meets, by brute force over the instances
	// binned to the cell under it (a vertical segment can only meet instances whose bounds cover it).
	void SegmentHitsBruteForce(const WorldIndex& a_world, const Float3& a_p, const Float3& a_q, std::vector<WorldSegmentHit>& a_hits);

	// True when a capsule query (world space) overlaps the instance's collision.
	[[nodiscard]] bool CapsuleOverlapsInstance(const WorldIndex& a_world, const Instance& a_instance, const Float3& a_p, const Float3& a_q, float a_radius);
}
