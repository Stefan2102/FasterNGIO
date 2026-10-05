#pragma once

#include "Collision/CollisionModel.h"
#include "Rejection/HlslShim.h"
#include "Rejection/WorldIndex.h"

#include <algorithm>
#include <cmath>

namespace FasterNGIO::Rejection
{
	// Exact capsule-vs-primitive tests in model space, on top of the shared GrassQueryMath.hlsli.
	// The brute-force reference and the CPU BVH both use these, so they agree hit for hit.

	[[nodiscard]] inline Hlsl::float3 ToHlsl(const Collision::Float3& a_value)
	{
		return { a_value.x, a_value.y, a_value.z };
	}

	[[nodiscard]] inline bool SegmentOverlapsAabb(const Hlsl::float3& a_p, const Hlsl::float3& a_q, float a_radius, const Collision::Float3& a_min, const Collision::Float3& a_max)
	{
		return (std::min)(a_p.x, a_q.x) - a_radius <= a_max.x && (std::max)(a_p.x, a_q.x) + a_radius >= a_min.x &&
		       (std::min)(a_p.y, a_q.y) - a_radius <= a_max.y && (std::max)(a_p.y, a_q.y) + a_radius >= a_min.y &&
		       (std::min)(a_p.z, a_q.z) - a_radius <= a_max.z && (std::max)(a_p.z, a_q.z) + a_radius >= a_min.z;
	}

	// A collision model's hull, as the shared hull tests read it.
	struct ModelHull
	{
		const Collision::CollisionModel& model;
		const Collision::Hull& hull;

		[[nodiscard]] Hlsl::uint PlaneCount() const { return hull.planeCount; }
		[[nodiscard]] Hlsl::float4 Plane(Hlsl::uint a_index) const
		{
			const auto& plane = model.hullPlanes[hull.firstPlane + a_index];
			return { plane.x, plane.y, plane.z, plane.w };
		}
		[[nodiscard]] Hlsl::uint FaceCount() const { return hull.triangleCount; }
		[[nodiscard]] Hlsl::HullFace Face(Hlsl::uint a_index) const
		{
			const auto& face = model.hullTriangles[hull.firstTriangle + a_index];
			return { ToHlsl(face.vertices[0]), ToHlsl(face.vertices[1]), ToHlsl(face.vertices[2]) };
		}
		[[nodiscard]] float Radius() const { return hull.radius; }
	};

	[[nodiscard]] inline bool HullContains(const Collision::CollisionModel& a_model, const Collision::Hull& a_hull, const Hlsl::float3& a_point)
	{
		return Hlsl::HullContainsPoint(ModelHull{ a_model, a_hull }, a_point);
	}

	[[nodiscard]] inline bool CapsuleOverlapsTriangle(const Collision::Triangle& a_triangle, const Hlsl::float3& a_p, const Hlsl::float3& a_q, float a_radius)
	{
		return Hlsl::CapsuleOverlapsTriangle(a_p, a_q, a_radius, ToHlsl(a_triangle.vertices[0]), ToHlsl(a_triangle.vertices[1]), ToHlsl(a_triangle.vertices[2]),
			a_triangle.radius);
	}

	[[nodiscard]] inline bool CapsuleOverlapsHull(const Collision::CollisionModel& a_model, const Collision::Hull& a_hull, const Hlsl::float3& a_p, const Hlsl::float3& a_q,
		float a_radius)
	{
		// The GPU gets the same early-out from the hull's BLAS AABB.
		if (!SegmentOverlapsAabb(a_p, a_q, a_radius + a_hull.radius, a_hull.aabbMin, a_hull.aabbMax)) {
			return false;
		}
		return Hlsl::CapsuleOverlapsHull(a_p, a_q, a_radius, ModelHull{ a_model, a_hull });
	}

	[[nodiscard]] inline bool CapsuleOverlapsCapsule(const Collision::Capsule& a_capsule, const Hlsl::float3& a_p, const Hlsl::float3& a_q, float a_radius)
	{
		return Hlsl::CapsuleOverlapsCapsule(a_p, a_q, a_radius, ToHlsl(a_capsule.p0), ToHlsl(a_capsule.p1), a_capsule.radius);
	}

	// Where segment [a_p, a_q] meets each kind of primitive (see GrassQueryMath.hlsli's SegmentHit).
	[[nodiscard]] inline Hlsl::SegmentHit SegmentHitTriangle(const Collision::Triangle& a_triangle, const Hlsl::float3& a_p, const Hlsl::float3& a_q)
	{
		return Hlsl::SegmentHitTriangle(a_p, a_q, ToHlsl(a_triangle.vertices[0]), ToHlsl(a_triangle.vertices[1]), ToHlsl(a_triangle.vertices[2]));
	}

	[[nodiscard]] inline Hlsl::SegmentHit SegmentHitHull(const Collision::CollisionModel& a_model, const Collision::Hull& a_hull, const Hlsl::float3& a_p, const Hlsl::float3& a_q)
	{
		if (!SegmentOverlapsAabb(a_p, a_q, a_hull.radius, a_hull.aabbMin, a_hull.aabbMax)) {
			return Hlsl::NoSegmentHit();
		}
		return Hlsl::SegmentHitHull(a_p, a_q, ModelHull{ a_model, a_hull });
	}

	[[nodiscard]] inline Hlsl::SegmentHit SegmentHitCapsule(const Collision::Capsule& a_capsule, const Hlsl::float3& a_p, const Hlsl::float3& a_q)
	{
		return Hlsl::SegmentHitCapsule(a_p, a_q, ToHlsl(a_capsule.p0), ToHlsl(a_capsule.p1), a_capsule.radius);
	}

	// A model-space hit on instance a_instance as a world-space one (t is unchanged by the transform).
	[[nodiscard]] inline WorldSegmentHit ToWorldHit(std::uint32_t a_instance, const Similarity& a_worldFromModel, const Hlsl::SegmentHit& a_hit)
	{
		const auto n = a_worldFromModel.ApplyLinear(Collision::Float3{ a_hit.normal.x, a_hit.normal.y, a_hit.normal.z });
		const auto length = std::sqrt(n.x * n.x + n.y * n.y + n.z * n.z);
		const auto scale = length > 0.0f ? 1.0f / length : 0.0f;
		return WorldSegmentHit{ .instance = a_instance, .t = a_hit.t, .normal = { n.x * scale, n.y * scale, n.z * scale } };
	}
}
