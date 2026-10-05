#include "Rejection/NgioRules.h"

#include "Collision/NifCollisionExtractor.h"
#include "GameData/Records.h"
#include "Rejection/HlslShim.h"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace FasterNGIO::Rejection
{
	static_assert(Hlsl::kRoleOrdinary == kRoleOrdinary && Hlsl::kRoleCliff == kRoleCliff && Hlsl::kRolePartIgnored == kRolePartIgnored);
	static_assert(Hlsl::kNoInstance == kNoInstance);

	namespace
	{
		using Collision::Float3;

		[[nodiscard]] InstanceRole RoleOf(const WorldIndex& a_world, std::uint32_t a_instance)
		{
			return a_instance == kNoInstance ? InstanceRole{} : a_world.Instances()[a_instance].role;
		}

		// The render shape of an instance nearest a world-space point.
		[[nodiscard]] const Collision::RenderShape* NearestShape(const WorldIndex& a_world, std::uint32_t a_instance, const Float3& a_point)
		{
			const auto& instance = a_world.Instances()[a_instance];
			return Collision::NearestRenderShape(a_world.Models()[instance.model].collision, instance.modelFromWorld.Apply(a_point));
		}

		[[nodiscard]] bool AnyNamed(const WorldIndex& a_world, std::uint32_t a_instance, const Collision::RenderShape& a_shape, const std::vector<std::string>& a_names)
		{
			const auto& model = a_world.Models()[a_world.Instances()[a_instance].model].collision;
			return std::ranges::any_of(a_names, [&](const std::string& a_name) { return Collision::RenderShapeNamed(model, a_shape, a_name); });
		}
	}

	CliffNeighbourSegments MakeCliffNeighbourSegments(const float (&a_position)[3], float a_cliffZ, float a_offset, bool a_steep)
	{
		const float window = a_steep ? Hlsl::kCliffSteepWindow : Hlsl::kCliffWindow;
		const float x = a_position[0];
		const float y = a_position[1];
		const std::array<std::array<float, 2>, 4> offsets{ { { a_offset, 0.0f }, { -a_offset, 0.0f }, { 0.0f, a_offset }, { 0.0f, -a_offset } } };
		CliffNeighbourSegments segments;
		for (std::size_t i = 0; i < offsets.size(); ++i) {
			segments.bottom[i] = { x + offsets[i][0], y + offsets[i][1], a_cliffZ - window };
			segments.top[i] = { x + offsets[i][0], y + offsets[i][1], a_cliffZ + window };
		}
		return segments;
	}

	float CliffNeighbourOffset(const GameData::GrassInfo& a_grass)
	{
		// As NGIO: the bounds count only when both X bounds are non-zero.
		const auto& bounds = a_grass.bounds;
		if (bounds.present && bounds.max[0] != 0 && bounds.min[0] != 0) {
			const auto halfX = std::abs(static_cast<float>(bounds.max[0] - bounds.min[0]) / 2.0f);
			const auto halfY = std::abs(static_cast<float>(bounds.max[1] - bounds.min[1]) / 2.0f);
			return (std::max)(halfX, halfY) + Hlsl::kCliffNeighbourMargin;
		}
		return Hlsl::kCliffNeighbourDefault;
	}

	RayReduction ReduceRayHits(const WorldIndex& a_world, const std::vector<WorldSegmentHit>& a_hits, bool a_cliffsOnly)
	{
		RayReduction result;
		for (const auto& hit : a_hits) {
			if (hit.t < result.closestT) {
				result.closestT = hit.t;
				result.closest = hit.instance;
			}
			if ((!a_cliffsOnly || RoleOf(a_world, hit.instance) == kRoleCliff) && hit.t > result.highestT) {
				result.highestT = hit.t;
				result.highestNormal = hit.normal;
			}
		}
		return result;
	}

	CliffRays TraceCliffRays(const SegmentHitsFunction& a_segmentHits, const WorldIndex& a_world, const float (&a_position)[3], float a_neighbourOffset)
	{
		CliffRays rays;
		std::vector<WorldSegmentHit> hits;
		const Float3 base{ a_position[0], a_position[1], a_position[2] };
		a_segmentHits(base, Float3{ base.x, base.y, base.z + Hlsl::kCliffRayLength }, hits);
		const auto up = ReduceRayHits(a_world, hits, true);
		rays.upClosest = up.closest;
		if (up.closest == kNoInstance || RoleOf(a_world, up.closest) != kRoleCliff || up.highestT < 0.0f) {
			return rays;
		}
		rays.cliffT = up.highestT;
		// Hits face against the ray; NGIO negates the normal so that it faces up the cliff.
		rays.cliffNormal = { -up.highestNormal.x, -up.highestNormal.y, -up.highestNormal.z };
		const auto cliffZ = base.z + Hlsl::kCliffRayLength * up.highestT;
		const auto segments = MakeCliffNeighbourSegments(a_position, cliffZ, a_neighbourOffset, a_world.Instances()[up.closest].steep);
		for (std::size_t i = 0; i < segments.bottom.size(); ++i) {
			hits.clear();
			a_segmentHits(segments.bottom[i], segments.top[i], hits);
			const auto neighbour = ReduceRayHits(a_world, hits, false);
			rays.neighbours[i] = CliffRay{ .closest = neighbour.closest, .highestT = neighbour.highestT };
		}
		return rays;
	}

	bool VolumeRejects(const VolumeHits& a_hits, const WorldIndex& a_world, const RejectionFeatures& a_features, const float (&a_position)[3])
	{
		if (a_hits.ordinary) {
			return true;
		}
		if (a_hits.partIgnored == kNoInstance) {
			return false;
		}
		const auto names = a_features.ignoredShapes.find(a_world.Instances()[a_hits.partIgnored].baseFormID);
		const auto* shape = NearestShape(a_world, a_hits.partIgnored, Float3{ a_position[0], a_position[1], a_position[2] });
		return names == a_features.ignoredShapes.end() || !shape || !AnyNamed(a_world, a_hits.partIgnored, *shape, names->second);
	}

	CliffDecision DecideCliff(const CliffRays& a_rays, const WorldIndex& a_world, const RejectionFeatures& a_features, const GameData::GrassInfo& a_grass,
		const float (&a_position)[3])
	{
		CliffDecision decision;
		// NGIO leaves grass that grows only under water where it is.
		if (a_grass.underwaterState == GameData::GrassWaterState::BelowOnlyAtLeast || a_grass.underwaterState == GameData::GrassWaterState::BelowOnlyAtMost) {
			return decision;
		}
		if (a_rays.upClosest == kNoInstance || RoleOf(a_world, a_rays.upClosest) != kRoleCliff || a_rays.cliffT < 0.0f) {
			return decision;
		}
		decision.outcome = CliffOutcome::Failed;
		const auto cliffZ = a_position[2] + Hlsl::kCliffRayLength * a_rays.cliffT;

		// The grass type's slope range, against the cliff surface (radians, as NGIO compares them).
		constexpr float kDegrees = std::numbers::pi_v<float> / 180.0f;
		const auto angle = std::acos(std::clamp(a_rays.cliffNormal.z, -1.0f, 1.0f));
		if (angle > static_cast<float>(a_grass.maxSlopeDegrees) * kDegrees || angle < static_cast<float>(a_grass.minSlopeDegrees) * kDegrees) {
			return decision;
		}

		// [CliffObjects] shape names, on the nearest object's shape closest to the cliff point.
		const auto& closest = a_world.Instances()[a_rays.upClosest];
		if (const auto cliff = a_features.cliffObjects.find(closest.baseFormID); cliff != a_features.cliffObjects.end()) {
			const auto& object = cliff->second;
			if (!object.allowedShapes.empty() || !object.blockedShapes.empty()) {
				if (const auto* shape = NearestShape(a_world, a_rays.upClosest, Float3{ a_position[0], a_position[1], cliffZ })) {
					if (!object.allowedShapes.empty() && !AnyNamed(a_world, a_rays.upClosest, *shape, object.allowedShapes)) {
						return decision;
					}
					if (AnyNamed(a_world, a_rays.upClosest, *shape, object.blockedShapes)) {
						return decision;
					}
				}
			}
		}

		// Every neighbour meets something: a cliff, or an object near the cliff point's height.
		const float window = closest.steep ? Hlsl::kCliffSteepWindow : Hlsl::kCliffWindow;
		for (const auto& neighbour : a_rays.neighbours) {
			if (neighbour.closest == kNoInstance) {
				return decision;
			}
			const auto highestZ = cliffZ - window + 2.0f * window * neighbour.highestT;
			if (RoleOf(a_world, neighbour.closest) != kRoleCliff && std::abs(highestZ - cliffZ) > Hlsl::kCliffNeighbourTolerance) {
				return decision;
			}
		}

		decision.outcome = CliffOutcome::Moved;
		decision.z = cliffZ;
		decision.normal = a_rays.cliffNormal;
		return decision;
	}

	BladeDecision DecideBlade(const VolumeHits& a_hits, const CliffRays& a_rays, const WorldIndex& a_world, const RejectionFeatures& a_features,
		const GameData::GrassInfo& a_grass, const float (&a_position)[3])
	{
		BladeDecision decision;
		if (NeedsCliffRays(a_hits, a_features)) {
			const auto cliff = DecideCliff(a_rays, a_world, a_features, a_grass, a_position);
			if (cliff.outcome == CliffOutcome::Moved) {
				decision.moved = true;
				decision.z = cliff.z;
				decision.normal = cliff.normal;
				return decision;
			}
			if (cliff.outcome == CliffOutcome::Failed) {
				decision.rejected = true;
				return decision;
			}
		}
		// A cliff hit while cliffs are off rejects like any other.
		decision.rejected = (a_hits.cliff && !a_features.cliffs) || VolumeRejects(a_hits, a_world, a_features, a_position);
		return decision;
	}
}
