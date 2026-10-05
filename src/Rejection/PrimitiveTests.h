#pragma once

#include "Collision/CollisionModel.h"
#include "Rejection/HlslShim.h"

#include <algorithm>

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
}
