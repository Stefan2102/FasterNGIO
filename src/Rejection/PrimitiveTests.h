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

	[[nodiscard]] inline bool HullContains(const Collision::CollisionModel& a_model, const Collision::Hull& a_hull, const Hlsl::float3& a_point)
	{
		for (std::uint32_t i = 0; i < a_hull.planeCount; ++i) {
			const auto& plane = a_model.hullPlanes[a_hull.firstPlane + i];
			if (Hlsl::OutsidePlane(Hlsl::float4{ plane.x, plane.y, plane.z, plane.w }, a_point)) {
				return false;
			}
		}
		return true;
	}

	[[nodiscard]] inline bool CapsuleOverlapsTriangle(const Collision::Triangle& a_triangle, const Hlsl::float3& a_p, const Hlsl::float3& a_q, float a_radius)
	{
		return Hlsl::CapsuleOverlapsTriangle(a_p, a_q, a_radius, ToHlsl(a_triangle.vertices[0]), ToHlsl(a_triangle.vertices[1]), ToHlsl(a_triangle.vertices[2]),
			a_triangle.radius);
	}

	[[nodiscard]] inline bool CapsuleOverlapsHull(const Collision::CollisionModel& a_model, const Collision::Hull& a_hull, const Hlsl::float3& a_p, const Hlsl::float3& a_q,
		float a_radius)
	{
		if (!SegmentOverlapsAabb(a_p, a_q, a_radius + a_hull.radius, a_hull.aabbMin, a_hull.aabbMax)) {
			return false;
		}
		if (HullContains(a_model, a_hull, a_p) || HullContains(a_model, a_hull, a_q)) {
			return true;
		}
		for (std::uint32_t i = 0; i < a_hull.triangleCount; ++i) {
			const auto& tri = a_model.hullTriangles[a_hull.firstTriangle + i];
			if (Hlsl::CapsuleOverlapsTriangle(a_p, a_q, a_radius, ToHlsl(tri.vertices[0]), ToHlsl(tri.vertices[1]), ToHlsl(tri.vertices[2]), a_hull.radius)) {
				return true;
			}
		}
		return false;
	}

	[[nodiscard]] inline bool CapsuleOverlapsCapsule(const Collision::Capsule& a_capsule, const Hlsl::float3& a_p, const Hlsl::float3& a_q, float a_radius)
	{
		return Hlsl::CapsuleOverlapsCapsule(a_p, a_q, a_radius, ToHlsl(a_capsule.p0), ToHlsl(a_capsule.p1), a_capsule.radius);
	}
}
